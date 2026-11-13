"""Visual reference and cover relation controls inside the existing position page."""
from PySide6.QtWidgets import QWidget, QFormLayout, QHBoxLayout, QLabel, QPushButton, QComboBox, QStackedWidget
from .widgets import DoubleSpinBox, SpinBox, parameter_row, sync_number, Disclosure


class VisualControls(QWidget):
    def __init__(self, session):
        super().__init__()
        self.session, self.loading, self.preview, self.region = session, False, None, None
        form = QFormLayout(self);form.setContentsMargins(0, 0, 0, 0)
        form.setVerticalSpacing(10);form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        self.advanced=Disclosure('高级定位设置')
        self._shown_preview=None
        advanced_form=QFormLayout();advanced_form.setVerticalSpacing(10)
        advanced_form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        self.advanced.body.layout().addLayout(advanced_form)
        self.kind=QComboBox()
        self.kind.addItem('文字／图案位置（无需边框）','features')
        self.kind.addItem('完整边框区域','boundary')
        form.addRow('参考内容',self.kind)
        self.search_limit=SpinBox();self.search_limit.setRange(32,500);self.search_limit.setValue(500);self.search_limit.setSuffix(' DIP')
        self.search_limit.setKeyboardTracking(False);self.search_limit.valueChanged.connect(self.search_limit_changed)
        advanced_form.addRow('找回距离',parameter_row(self.search_limit,'最大找回距离',slider=False))
        self.check_interval=QComboBox()
        for seconds,title in ((0,'关闭，仅窗口变化时检查'),(15,'每 15 秒（默认）'),(30,'每 30 秒'),(60,'每 60 秒')):self.check_interval.addItem(title,seconds)
        self.check_interval.setCurrentIndex(1)
        self.check_interval.currentIndexChanged.connect(self.interval_changed)
        self.check_interval.setToolTip('全局设置：窗口尺寸或DPI改变时仍会定位。参考区域被挡住时跳过检查，保留上次位置。')
        advanced_form.addRow('兜底检查',self.check_interval)
        self.refresh_now=QPushButton('重新定位')
        self.refresh_now.setToolTip('保存并退出编辑后重新定位，不重新框选参考区域。')
        self.refresh_now.clicked.connect(session.refresh_visual)
        self.begin = QPushButton('选择参考区域…')
        self.begin.clicked.connect(lambda:session.calibration_action('begin'))
        tools=QWidget();tools_layout=QHBoxLayout(tools);tools_layout.setContentsMargins(0,0,0,0)
        tools_layout.addWidget(self.begin,1);tools_layout.addWidget(self.refresh_now)
        form.addRow(tools)
        self.resume=QPushButton('恢复已保存的视觉校准')
        self.resume.clicked.connect(session.resume_visual);self.resume.hide();form.addRow(self.resume)
        self.retry=QPushButton('重试刚才的校准')
        self.retry.clicked.connect(lambda:session.calibration_action('retry'));self.retry.hide();form.addRow(self.retry)
        self.summary = QLabel('青框：识别参考；绿框：实际遮盖；灰框：允许搜索的范围。')
        self.summary.setWordWrap(True);self.summary.setObjectName('hint');form.addRow(self.summary)
        self.details = QWidget();details = QFormLayout(self.details);details.setContentsMargins(0,0,0,0)
        details.setVerticalSpacing(8);details.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
        self.reference_controls = {}
        self.reference_rows=[]
        for keys, labels in ((('x','y'),('左','上')),(('width','height'),('宽','高'))):
            row = QWidget();layout = QHBoxLayout(row);layout.setContentsMargins(0,0,0,0)
            for key,label in zip(keys,labels):
                spin = DoubleSpinBox();spin.setRange(8 if key in ('width','height') else 0,100000)
                spin.setDecimals(2);spin.setKeyboardTracking(False)
                spin.valueChanged.connect(self.reference_changed)
                self.reference_controls[key]=spin;layout.addWidget(QLabel(label));layout.addWidget(parameter_row(spin,label,slider=False,compact=True))
            details.addRow('参考框',row)
            self.reference_rows.append(row)
        self.mode = QComboBox()
        self.mode.addItem('只跟位置，保持遮盖大小','position_only')
        self.mode.addItem('跟随参考框四边','follow_bounds')
        self.mode.currentIndexChanged.connect(self.changed)
        details.addRow('遮盖方式',self.mode)
        self.pages = QStackedWidget();details.addRow(self.pages)
        self.anchors, self.numbers = {}, {}
        position = QWidget();pf = QFormLayout(position);pf.setContentsMargins(0,0,0,0)
        for key,label,values in (('anchor_x','水平基准',(('left','左'),('center','中'),('right','右'))),
                                 ('anchor_y','垂直基准',(('top','上'),('center','中'),('bottom','下')))):
            combo=QComboBox()
            for value,title in values:combo.addItem(title,value)
            combo.currentIndexChanged.connect(self.anchor_changed)
            self.anchors[key]=combo;pf.addRow(label,combo)
        self.add_pair(pf,'相对偏移',('offset_x','offset_y'),('X','Y'),-100000,100000)
        self.add_pair(pf,'固定尺寸',('width','height'),('宽','高'),8,100000)
        self.pages.addWidget(position)
        bounds=QWidget();bf=QFormLayout(bounds);bf.setContentsMargins(0,0,0,0)
        self.add_pair(bf,'边距',('left','top'),('左','上'),-4096,4096)
        self.add_pair(bf,'边距',('right','bottom'),('右','下'),-4096,4096)
        padding=SpinBox();padding.setRange(0,2);padding.setSuffix(' 物理像素');padding.valueChanged.connect(self.changed)
        self.numbers['padding']=padding;bf.addRow('防漏边余量',parameter_row(padding,'防漏边余量',slider=False))
        note=QLabel('正边距向外扩，负边距向内收；字体大小保持不变。');note.setWordWrap(True);note.setObjectName('hint');bf.addRow(note)
        self.pages.addWidget(bounds)
        self.actions=QWidget();actions=QHBoxLayout(self.actions);actions.setContentsMargins(0,0,0,0)
        self.confirm=QPushButton('确认校准');self.confirm.setObjectName('primary');self.cancel=QPushButton('取消')
        self.reselect=QPushButton('重选')
        for button,op in ((self.confirm,'confirm'),(self.cancel,'cancel'),(self.reselect,'pick')):
            button.clicked.connect(lambda checked=False,op=op:session.calibration_action(op));actions.addWidget(button)
        form.insertRow(0,self.actions);form.addRow(self.details);form.addRow(self.advanced)
        self.details.hide();self.actions.hide()

    def add_pair(self,form,label,keys,labels,minimum,maximum):
        row=QWidget();layout=QHBoxLayout(row);layout.setContentsMargins(0,0,0,0)
        for key,title in zip(keys,labels):
            spin=DoubleSpinBox();spin.setRange(minimum,maximum);spin.setDecimals(2);spin.setKeyboardTracking(False)
            spin.valueChanged.connect(self.changed);self.numbers[key]=spin
            layout.addWidget(QLabel(title));layout.addWidget(parameter_row(spin,title,slider=False,compact=True))
        form.addRow(label,row)

    def snapshot(self,region,preview,available):
        force=self.region is None or self.region.id!=region.id
        self.loading=True;self.region=region;self.preview=preview
        token=preview.get('token') if preview else None
        if force:self.advanced.set_expanded(False)
        if token and token!=self._shown_preview:self.advanced.set_expanded(True)
        self._shown_preview=token
        self.summary.setVisible(True)
        self.check_interval.setCurrentIndex(self.check_interval.findData(getattr(self.session,'visual_check_interval',15)))
        self.refresh_now.setEnabled(available and preview is None and region.position_mode=='visual')
        compatible=getattr(self.session,'visual_preview_available',False)
        feature_compatible=getattr(self.session,'feature_position_available',False)
        kind=preview.get('tracking_kind','boundary') if preview else region.visual_binding.get('tracking_kind','boundary') if region.visual_binding else self.kind.currentData()
        if kind=='features' and not feature_compatible:kind='boundary'
        self.kind.setCurrentIndex(self.kind.findData(kind))
        self.kind.setEnabled(preview is None)
        self.kind.model().item(0).setEnabled(feature_compatible)
        self.search_limit.setEnabled(kind=='features' and preview is None)
        binding=region.visual_binding or {}
        if preview:sync_number(self.search_limit,round(preview.get('search_limit_dip',500)),force)
        elif binding:sync_number(self.search_limit,round(binding.get('search_limit_dip',500)),force)
        self.mode.model().item(1).setEnabled(kind!='features')
        count=sum(r.position_mode=='visual' for r in self.session.regions)
        self.begin.setEnabled(available and compatible and preview is None and (region.position_mode=='visual' or count<4))
        self.resume.setVisible(region.position_mode=='anchor' and region.visual_binding is not None and preview is None)
        self.resume.setEnabled(available and compatible and count<4)
        self.retry.setVisible(getattr(self.session,'calibration_retry_available',False) and preview is None)
        relation=preview['relation'] if preview else region.visual_binding['relation'] if region.visual_binding else None
        reference=preview.get('reference') if preview else None
        self.actions.setVisible(preview is not None)
        self.details.setVisible((preview is not None and reference is not None) or region.position_mode=='visual')
        self.details.setEnabled(compatible)
        self.confirm.setEnabled(reference is not None)
        for row in self.reference_rows:
            row.setVisible(preview is not None)
            label=self.details.layout().labelForField(row)
            if label:label.setVisible(preview is not None)
        for key,spin in self.reference_controls.items():
            if reference is not None:sync_number(spin,reference[('x','y','width','height').index(key)],force)
        if relation:
            self.mode.setCurrentIndex(self.mode.findData(relation['mode']))
            self.pages.setCurrentIndex(0 if relation['mode']=='position_only' else 1)
            for key,combo in self.anchors.items():combo.setCurrentIndex(combo.findData(relation[key]))
            for key,value in zip(('offset_x','offset_y','width','height','left','top','right','bottom','padding'),
                [*relation['offset_dip'],*relation['fixed_size_dip'],*relation['margins_dip'],relation['padding_physical']]):
                sync_number(self.numbers[key],value,force)
        if preview:
            self.summary.setText(('正在框选参考区域，Esc 可取消。' if preview['picking'] else '尚未选定参考区域，请点击“重新框选”；确认前不会启用视觉跟踪。') if reference is None else
                '预览尚未保存。青框是参考，绿框是遮盖。确认后面板短暂隐藏并切回目标进行校准；成功或失败都会返回面板显示结果。请保持目标前台，修改其它属性或窗口大小会取消本次预览。')
        else:self.summary.setText('移动窗口直接跟随；尺寸或 DPI 变化后重新定位。' if region.position_mode=='visual' else '当前使用窗口锚定。选择参考区域并确认后，启用视觉定位。')
        self.loading=False

    def relation(self):
        return dict(mode=self.mode.currentData(),anchor_x=self.anchors['anchor_x'].currentData(),
            anchor_y=self.anchors['anchor_y'].currentData(),
            offset_dip=[self.numbers[k].value() for k in ('offset_x','offset_y')],
            fixed_size_dip=[self.numbers[k].value() for k in ('width','height')],
            margins_dip=[self.numbers[k].value() for k in ('left','top','right','bottom')],
            padding_physical=self.numbers['padding'].value())

    def changed(self,*args):
        if self.loading:return
        self.pages.setCurrentIndex(0 if self.mode.currentData()=='position_only' else 1)
        self.session.update_visual_relation(self.relation())

    def anchor_changed(self,*args):
        if self.loading:return
        # Rebase the offset so changing the reference edge does not jump the cover.
        old=self.preview['relation'] if self.preview else self.region.visual_binding['relation']
        frozen=getattr(self.session,'editing_preview',None)
        ref=self.preview['reference'] if self.preview else frozen['reference'] if frozen and frozen['id']==self.region.id else self.region.visual_binding['reference_rect_dip']
        factors=dict(left=0,top=0,center=.5,right=1,bottom=1)
        self.loading=True
        for axis,key,number,size in ((0,'anchor_x','offset_x',ref[2]),(1,'anchor_y','offset_y',ref[3])):
            value=old['offset_dip'][axis]+size*(factors[old[key]]-factors[self.anchors[key].currentData()])
            self.numbers[number].setValue(value)
        self.loading=False;self.changed()

    def reference_changed(self,*args):
        if not self.loading and self.preview:
            self.session.set_calibration_reference([self.reference_controls[k].value() for k in ('x','y','width','height')])

    def search_limit_changed(self,*args):
        if not self.loading and self.region and self.region.visual_binding and self.region.visual_binding.get('tracking_kind')=='features':
            self.session._send('set_visual_search_limit',dict(id=self.region.id,limit=self.search_limit.value()))

    def interval_changed(self,*args):
        if not self.loading:self.session._send('set_visual_check_interval',dict(seconds=self.check_interval.currentData()))

    def commit(self):
        for widget in [self.search_limit,*self.reference_controls.values(),*self.numbers.values()]:widget.interpretText()
