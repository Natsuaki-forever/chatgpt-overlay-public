"""On-demand property editor backed by the native engine's authoritative session."""
from dataclasses import asdict, replace
import queue
import threading
from PySide6.QtCore import QByteArray, QObject, QThread, Qt, Signal, Slot
from PySide6.QtGui import QWindow
from .model import Region
from .pipe_client import PipeClient
from . import native

from .issue_history import IssueLog, OPERATION_LABELS, describe_issue


class SessionWorker(QThread):
    reply = Signal(object, object)
    failed = Signal(str)

    def __init__(self, pipe, token, hwnd):
        super().__init__()
        self.pipe, self.token, self.hwnd = pipe, token, hwnd
        self.requests = queue.Queue()
        self.stopping = threading.Event()
        self.client = None

    def submit(self, item):
        self.requests.put(item)

    def stop(self):
        self.stopping.set()
        client = self.client
        if client:
            client.cancel()
        self.requests.put(None)

    def run(self):
        revision, sequence, request_id = 0, -1, 0
        item = dict(op='hello', payload=dict(hwnd=self.hwnd, capabilities=['visual_geometry_v1', 'visual_tracking_v1', 'visual_calibration_preview_v1']), tag=0)
        try:
            with PipeClient(self.pipe) as client:
                self.client = client
                while not self.stopping.is_set():
                    if item['op'] == 'commit_close':
                        # Native gestures may have advanced the revision since the
                        # previous poll; finish against a fresh authoritative state.
                        request_id += 1
                        refreshed = client.exchange(dict(protocol=1, session=self.token,
                            request_id=request_id, op='snapshot', payload={}))
                        if refreshed.get('request_id') != request_id or not refreshed.get('ok'):
                            raise ValueError('Cannot confirm the final engine state')
                        revision = refreshed['revision']
                        sequence = refreshed['state']['state_sequence']
                        self.reply.emit(dict(op='snapshot', tag=None), refreshed)
                    request_id += 1
                    # Every envelope carries the latest authoritative revision.
                    # Read operations ignore it; new write operations cannot omit it.
                    message = dict(protocol=1, session=self.token, request_id=request_id,
                        op=item['op'], payload=item.get('payload', {}),base_revision=revision)
                    result = client.exchange(message)
                    if result.get('request_id') != request_id:
                        raise ValueError('Local session response ID mismatch')
                    revision = result.get('revision', revision)
                    if 'state' in result:
                        sequence = result['state']['state_sequence']
                    if not result.get('ok'):
                        # Unsent changes cannot silently rebase onto a rejected revision.
                        cancelled = []
                        while True:
                            try:
                                queued = self.requests.get_nowait()
                            except queue.Empty:
                                break
                            if queued and queued['op'] not in ('snapshot', 'select', 'commit_close'):
                                cancelled.append(queued['op'])
                        item = dict(item, cancelled_operations=cancelled)
                    self.reply.emit(item, result)
                    if item['op'] == 'hello' and not result.get('ok'):
                        return
                    if item['op'] == 'commit_close' and result.get('ok'):
                        return
                    try:
                        item = self.requests.get(timeout=.1)
                    except queue.Empty:
                        item = dict(op='snapshot', payload=dict(state_sequence=sequence), tag=None)
                    if item is None:
                        return
        except Exception as error:
            if not self.stopping.is_set():
                self.failed.emit(str(error))
        finally:
            self.client = None


class RemoteSession(QObject):
    supports_anchors = True
    def __init__(self, app, pipe, token):
        super().__init__()
        from .ui import Inspector
        self.app = app
        self.target = None
        self._regions = []
        self._selected = None
        self._serial = 0
        self.pending = {}
        self.pending_selection = None
        self.closing = False
        self.error = ''
        self._save_revisions = (0, 0)
        self._commit_save_failed = False
        self.issues = None
        self.host_save_error = ''
        self.host_render_error = ''
        self.host_pick_error = ''
        self.finish_sequence = 0
        self.focus_sequence = 0
        self.owner_window = None
        self.inspector = Inspector(self)
        self.inspector.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.worker = SessionWorker(pipe, token, int(self.inspector.winId()))
        self.worker.reply.connect(self._reply)
        self.worker.failed.connect(self._failed)
        self.worker.finished.connect(app.quit)
        app.aboutToQuit.connect(self.worker.stop)

    @property
    def regions(self):
        return [replace(region) for region in self._regions]

    @property
    def selection(self):
        return next((i for i, region in enumerate(self._regions) if region.id == self._selected), -1)

    @selection.setter
    def selection(self, index):
        identifier = self._regions[index].id if 0 <= index < len(self._regions) else None
        if identifier == self._selected:
            return
        self._selected = identifier
        tag = self._send('select', dict(id=identifier))
        self.pending_selection = (tag, identifier)

    def _send(self, op, payload=None):
        self._serial += 1
        self.worker.submit(dict(op=op, payload=payload or {}, tag=self._serial))
        return self._serial

    def update_region(self, index, values):
        if self.closing or not 0 <= index < len(self._regions):
            return
        original = self._regions[index]
        updated = Region.parse(dict(asdict(original), **{key: value for key, value in values.items() if key != 'id'}))
        changes = {key: value for key, value in asdict(updated).items() if key != 'id' and getattr(original, key) != value}
        if not changes:
            return
        self._regions[index] = updated
        self.error = ''
        tag = self._send('update', dict(id=original.id, values=changes, coordinate_space='client'))
        self.pending[tag] = (original.id, changes)
        self.refresh_save_status()

    def delete_region(self, index):
        if not self.closing and 0 <= index < len(self._regions):
            self._send('delete', dict(id=self._regions[index].id))

    def create_region(self):
        if not self.closing:
            if getattr(self, 'canvas_modes', False):
                self.set_canvas_mode('once')
            else:
                self._send('create',dict(x=32,y=32,width=160,height=40,text='新区域'))

    def set_canvas_mode(self, mode):
        if not self.closing:
            self._send('set_canvas_mode', dict(mode=mode))

    def repaint(self):
        pass  # The engine paints; the editor never owns a normal overlay surface.

    def select_target(self, hwnd):
        self._send('select_target', dict(hwnd=int(hwnd)))

    def pick_color(self, field='background'):
        if self._selected:
            self._send('pick_color', dict(id=self._selected, field=field))

    def visual_action(self, action):
        if self._selected and not self.closing:
            self.inspector.commit()
            self._send('visual_'+action, dict(id=self._selected, use_saved_position=True))

    def refresh_visual(self):
        if self._selected and not self.closing:
            engine=getattr(self,'engine_pid',0)
            if engine:native.u.AllowSetForegroundWindow(engine)
            self.inspector.commit()
            self._send('refresh_visual',dict(id=self._selected))
            self.set_editing(False)

    def _hide_calibration_operation(self,op,payload=None,kind='check'):
        self.calibration_hide_kind=kind;self.calibration_hide_ack=False
        self.inspector.hide()
        if kind=='check' and self.target:
            engine_pid=getattr(self,'engine_pid',0)
            if engine_pid:native.u.AllowSetForegroundWindow(engine_pid)
            native.u.SetForegroundWindow(self.target)
        self.calibration_hide_tag=self._send(op,payload)

    def calibration_action(self,action):
        if not self._selected or self.closing:return
        self.error=''
        self.inspector.commit()
        if action=='begin':
            kind=self.inspector.visual_controls.kind.currentData()
            limit=self.inspector.visual_controls.search_limit.value()
            self._hide_calibration_operation('begin_calibration',dict(id=self._selected,tracking_kind=kind,search_limit_dip=limit),'reference');return
        if action=='retry':self._hide_calibration_operation('retry_calibration');return
        preview=getattr(self,'calibration_preview',None)
        if preview:
            op={'confirm':'confirm_calibration','cancel':'cancel_calibration','pick':'pick_calibration_reference'}[action]
            if action in ('confirm','pick'):
                self._hide_calibration_operation(op,dict(token=preview['token']),'reference' if action=='pick' else 'check')
            else:self._send(op,dict(token=preview['token']))

    def set_calibration_reference(self,rect):
        preview=getattr(self,'calibration_preview',None)
        if preview and not self.closing:self._send('set_calibration_reference',dict(token=preview['token'],rect=rect))

    def update_visual_relation(self,relation):
        if self.closing or not self._selected:return
        self.error=''
        preview=getattr(self,'calibration_preview',None)
        if preview:self._send('update_calibration_preview',dict(token=preview['token'],relation=relation))
        else:self._send('update_cover_relation',dict(id=self._selected,relation=relation))

    def resume_visual(self):
        if self._selected and not self.closing:
            self.inspector.commit();self.error=''
            self._hide_calibration_operation('set_position_mode',dict(id=self._selected,mode='visual'))

    def set_editing(self, value):
        if value or self.closing:
            return
        self.inspector.commit()
        self.error = ''
        self._commit_save_failed = False
        self.closing = True
        state = self.inspector.capture_state()
        self._send('commit_close', dict(editor_state=dict(page=state['page'], geometry=bytes(state['geometry'].toBase64()).decode('ascii'))))
        self.inspector.setEnabled(False)
        self.refresh_save_status()
        self.inspector.set_feedback('正在保存最后一次修改…')

    def refresh_save_status(self):
        self.inspector.set_save_status(
            *self._save_revisions,
            error=self.host_save_error or self._commit_save_failed,
            awaiting=bool(self.pending) or self.closing,
            rejected=bool(self.error),
        )

    @Slot(object, object)
    def _reply(self, item, result):
        tag = item.get('tag')
        state = result.get('state')
        if self.issues is None and state and state.get('diagnostics_dir'):
            self.issues = IssueLog(state['diagnostics_dir'])
        if getattr(self,'calibration_hide_kind',None) and tag==getattr(self,'calibration_hide_tag',None):
            self.calibration_hide_ack=True
            if not result.get('ok'):self.calibration_hide_kind=None
        self.pending.pop(tag, None)
        if self.pending_selection and self.pending_selection[0] == tag:
            self.pending_selection = None
        if not result.get('ok'):
            self.pending.clear()
            self.pending_selection = None
            self.closing = False
            self.inspector.setEnabled(True)
            code=result.get('error','unknown error')
            self._commit_save_failed = code == 'save_failed'
            operation = OPERATION_LABELS.get(item['op'], '当前操作')
            _, advice = describe_issue(code)
            self.error = f'{operation}失败。{advice.rstrip("。 ")}。'
            cancelled = item.get('cancelled_operations', [])
            if self.issues:
                self.issues.write(code, item['op'], cancelled)
            if cancelled:
                names = '、'.join(dict.fromkeys(OPERATION_LABELS.get(op, '其他操作') for op in cancelled))
                self.error += f'另有 {len(cancelled)} 项未提交（{names}），请检查后重做。'
        if state is not None:
            before = ([asdict(region) for region in self._regions], self._selected)
            previous_tracking_states = getattr(self, 'tracking_states', {})
            previous_preview=getattr(self,'calibration_preview',None)
            previous_retry=getattr(self,'calibration_retry_available',False)
            self.calibration_preview=state.get('calibration_preview')
            self.calibration_retry_available=state.get('calibration_retry_available',False)
            self.editing_preview=state.get('editing_preview')
            regions = [Region.parse(region) for region in state['regions']]
            positions = state.get('effective_positions', {})
            self.tracking_states = state.get('tracking_states', {})
            rectangles = state.get('effective_rects', {})
            for region in regions:
                if region.position_mode == 'visual':
                    rect = rectangles.get(region.id)
                    if rect:
                        region.x, region.y = rect['x'], rect['y']
                        region.width, region.height = rect['width'], rect['height']
                elif region.id in positions:
                    region.x = positions[region.id]['x']
                    region.y = positions[region.id]['y']
            for identifier, changes in self.pending.values():
                for index, region in enumerate(regions):
                    if region.id == identifier:
                        regions[index] = Region.parse(dict(asdict(region), **changes))
            self._regions = regions
            self._selected = self.pending_selection[1] if self.pending_selection else state.get('selected_id')
            previous_target = self.target
            self.target = state.get('target_hwnd') or None
            self.application_id = state.get('application_id', '')
            self.application_name = state.get('application_name', 'ChatGPT')
            if self.target != previous_target:
                self.inspector.refresh_targets()
            self.host_save_error = state.get('save_error', '')
            self.host_render_error = state.get('render_error', '')
            self.host_pick_error = state.get('pick_error', '')
            visual_error = state.get('visual_error', '')
            if self.issues and visual_error and visual_error != getattr(self, 'visual_error', ''):
                self.issues.write(visual_error, 'visual_calibrate')
            self.visual_error = visual_error
            self.engine_pid=state.get('engine_pid',0)
            previous_waiting=getattr(self,'waiting_for_target',False)
            self.waiting_for_target=state.get('waiting_for_target',False)
            self.canvas_modes = state.get('canvas_modes', False)
            self.canvas_mode = state.get('canvas_mode', 'idle')
            self.inspector.free_region_button.setVisible(self.canvas_modes)
            self.inspector.free_region_button.setEnabled(not self.waiting_for_target)
            self.inspector.free_region_button.setText('结束框选' if self.canvas_mode == 'free' else '全窗口框选')
            self.inspector.add_button.setEnabled(not self.canvas_modes or not self.waiting_for_target)
            previous_interval=getattr(self,'visual_check_interval',15)
            self.visual_check_interval=state.get('visual_check_interval_seconds',15)
            self.calibration_result=state.get('calibration_result','')
            self.visual_worker_available = state.get('visual_worker_available', False)
            self.visual_preview_available='visual_calibration_preview_v1' in state.get('visual_capabilities',[])
            self.feature_position_available='feature_position_v1' in state.get('visual_capabilities',[])
            self.inspector.connection_status.setText(
                self.application_name if self.canvas_modes and not self.application_id else
                ('等待 ' if self.waiting_for_target else '已连接 ') + self.application_name)
            self._save_revisions = (state.get('revision', 0), state.get('persisted_revision', 0))
            if before != ([asdict(region) for region in regions], self._selected) or previous_tracking_states != self.tracking_states or previous_preview!=self.calibration_preview or previous_retry!=self.calibration_retry_available or previous_waiting!=self.waiting_for_target or previous_interval!=self.visual_check_interval or item['op'] == 'hello':
                self.inspector.refresh()
            if item['op'] == 'hello':
                if state.get('owner_hwnd'):
                    self.owner_window = QWindow.fromWinId(state['owner_hwnd'])
                    self.inspector.windowHandle().setTransientParent(self.owner_window)
                editor_state = state.get('editor_state', {})
                if editor_state.get('geometry'):
                    self.inspector.restore_state(dict(page=editor_state.get('page', 0), geometry=QByteArray.fromBase64(editor_state['geometry'].encode('ascii'))))
                else:
                    self.inspector.move_to_target()
            visible = state.get('visible', True)
            restore_sequence=state.get('editor_restore_sequence',0)
            explicit_restore=restore_sequence!=getattr(self,'editor_restore_sequence',0)
            self.editor_restore_sequence=restore_sequence
            if explicit_restore:self.calibration_hide_kind=None
            calibration_returned = False
            if getattr(self,'calibration_hide_kind',None):
                busy=bool((self.calibration_preview or {}).get('picking')) if self.calibration_hide_kind=='reference' else state.get('calibration_busy',False)
                if self.calibration_hide_ack and not busy:
                    calibration_returned = True
                    self.calibration_hide_kind=None
                else:visible=False
            if visible and not self.closing:
                if not self.inspector.isVisible():
                    self.inspector.show()
                elif not native.u.IsWindowVisible(int(self.inspector.winId())):
                    # A brief native hide/restore can occur between state polls;
                    # QWidget's cached visible flag then remains true.
                    native.u.ShowWindow(int(self.inspector.winId()), 4)
                if explicit_restore:
                    # External ShowWindow calls need not update Qt's cached state.
                    hwnd=int(self.inspector.winId())
                    if self.inspector.isMinimized() or native.u.IsIconic(hwnd):
                        self.inspector.showNormal()
                        if native.u.IsIconic(hwnd):native.u.ShowWindow(hwnd,9)
                if item['op'] == 'hello' or calibration_returned or explicit_restore:
                    self.inspector.raise_()
                if item['op'] == 'hello' or explicit_restore:
                    self.inspector.activateWindow()
            elif not visible:
                self.inspector.hide()
            finish = state.get('finish_sequence', 0)
            if finish != self.finish_sequence:
                self.finish_sequence = finish
                self.set_editing(False)
            focus = state.get('focus_text_sequence', 0)
            if focus != self.focus_sequence:
                self.focus_sequence = focus
                self.inspector.activateWindow()
                self.inspector.text.setFocus()
                self.inspector.text.selectAll()
        self.refresh_save_status()
        if self.error:
            self.inspector.set_feedback(self.error,'error')
        elif self.host_save_error:
            self.inspector.set_feedback('自动保存暂未完成，修改仍保留，正在重试。','error')
        elif self.host_render_error:
            self.inspector.set_feedback('覆盖绘制未完成：'+self.host_render_error,'error')
        elif self.host_pick_error:
            self.inspector.set_feedback('取色未完成：'+self.host_pick_error,'error')
        elif getattr(self, 'visual_error', ''):
            _, advice = describe_issue(self.visual_error)
            self.inspector.set_feedback(f'视觉跟踪未完成。{advice.rstrip("。 ")}。原校准与样式保留。', 'error')
        elif getattr(self,'calibration_result','')=='saved':
            self.inspector.set_feedback('视觉跟踪已启用并保存。点击“完成”退出编辑后开始跟踪。','success')
        elif self.closing:
            self.inspector.set_feedback('正在保存最后一次修改…')
        else:
            self.inspector.set_feedback(self.inspector.selection_hint())
        if item['op'] == 'commit_close' and result.get('ok'):
            self.inspector.hide()
        if item['op'] in ('visual_calibrate', 'visual_retry') and result.get('ok'):
            self.set_editing(False)

    @Slot(str)
    def _failed(self, text):
        if self.issues:
            self.issues.write('editor_transport_failed')
        self.error = text
        self.inspector.hide()

    def start(self):
        self.worker.start()
