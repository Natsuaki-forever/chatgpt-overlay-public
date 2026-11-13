"""Bounded local problem history. Only known codes and operation names are recorded."""
from datetime import datetime, timezone
import json
from pathlib import Path

VISUAL_ERRORS = {
    'revision_conflict': '配置刚发生变化，请检查当前值后重试',
    'target_unavailable': '请先打开或还原目标窗口，再选择参考区域',
    'invalid_target': '该窗口已关闭或暂不可用，请到「附着窗口」重新选择',
    'region_not_found': '该区域已移除，请重新选择要操作的区域',
    'template_unavailable': '校准参考图像缺失或校验失败，请重新框选',
    'visual_result_invalid': '识别返回了无效坐标，已停止显示，请重试校准',
    'visual_position_not_ready': '当前没有可用的遮盖位置，请等待定位或使用保存坐标切回窗口锚定',
    'visual_geometry_unavailable': '遮盖位置未能通过校验，请使用位置修复版查看具体原因',
    'feature_position_requires_fixed_size': '文字／图案定位只支持固定遮盖尺寸',
    'feature_reference_too_small': '参考内容过小，请稍微扩大选区，至少12×12物理像素',
    'feature_reference_too_large': '参考图像过大，请只选择一小段独特的文字或图案，无需包含外框',
    'ValueError:template-pixel-budget-exceeded': '参考图像过大，请缩小文字／图案选区，无需包含外框',
    'ValueError:insufficient-stable-features': '选区缺少清晰的文字或图案，请重新选择独特且稳定的内容',
    'ambiguous-features': '附近出现多个相似目标，请选择更独特的一段文字或图案',
    'visual_worker_component_mismatch': '识别组件版本不匹配，请使用完整的新版本目录',
    'reference_selection_interrupted': '框选被中断，请点击“重新框选”后再试',
    'visual_worker_missing': '缺少识别组件，请从完整解压的当前版启动，保留 vision 文件夹',
    'visual_worker_exited': '识别进程已退出，可点击重试',
    'visual_worker_timeout': '识别响应超时，可点击重试',
    'capture_permission_revoked': '请把目标窗口切到前台并移开遮挡，再重新定位',
    'calibration_context_changed': '校准时窗口或配置发生了变化，请重新校准',
    'calibration_wait_expired': '5 秒内未能切回目标窗口。请先点击目标窗口，再返回这里重试',
    'capture_pixels_not_clean': '系统未能可靠排除覆盖层，请保持画面静止后重试',
    'capture_positive_control_invisible': '系统采样未看到测试色块，无法验证覆盖层排除能力；请保持目标无遮挡后重试',
    'capture_positive_control_failed': '系统无法显示校准测试色块，请重新校准',
    'capture_exclusion_failed': '系统无法排除覆盖窗口，暂不能安全启用视觉跟踪',
    'visual_worker_handshake_timeout': '识别组件启动超时，请重试',
    'visual_worker_launch_failed': '识别组件无法启动，请检查组件是否完整',
    'capture_environment_changed_retry_required': '显示环境已变化，需要重新检查，请点击重试',
    'capture_surface_changed_retry_required': '覆盖窗口已变化，需要重新检查，请点击重试',
    'visual_roi_budget_exceeded': '参考区域过大，请缩小矩形',
    'boundary-unobservable-or-ambiguous': '没有找到唯一且完整的边框，请重新框选',
    'features-lost': '参考文字特征不足，请重新框选',
    'Cannot replace configuration': '校准保存失败，原设置已保留；请点击重试刚才的校准',
    'calibration_cancelled': '校准已取消，原设置已保留',
    'calibration_token_invalid': '预览已失效，请重新框选',
    'invalid_visual_cover_size': '当前边距使遮盖过小，请增大尺寸或减小负边距',
    'cover_outside_client': '遮盖完全超出目标窗口，请调整偏移或边距',
    'reference_outside_client': '参考框超出目标窗口，请重新框选或调整坐标',
    'calibration_retry_unavailable': '刚才的校准已失效，请重新框选',
    'reference_required': '请先框选完整的参考区域',
    'calibration_in_progress': '已有校准正在进行，请先确认或取消',
    'invalid_visual_number': '数值超出了允许范围',
    'manual_recalibration_required': '该参考已需要人工重选，请点击框选独立参考区域',
    'visual_region_limit': '最多启用四个视觉区域，请先停用一个区域',
}

OPERATION_LABELS = {
    'update': '编辑区域', 'create': '新建区域', 'delete': '删除区域',
    'select': '选择区域', 'select_target': '切换附着窗口', 'pick_color': '取色',
    'set_visual_check_interval': '设置定时检查', 'set_visual_search_limit': '设置搜索范围',
    'refresh_visual': '重新定位', 'set_position_mode': '切换定位方式',
    'visual_calibrate': '校准视觉定位', 'visual_retry': '重试视觉定位',
    'visual_anchor': '切回窗口锚定', 'begin_calibration': '开始框选',
    'pick_calibration_reference': '重新框选', 'set_calibration_reference': '应用参考框',
    'update_calibration_preview': '调整校准预览', 'confirm_calibration': '确认校准',
    'cancel_calibration': '取消校准', 'retry_calibration': '重试校准',
    'update_cover_relation': '修改遮盖偏移', 'commit_close': '保存并退出',
}

ISSUES = {
    'save_failed': ('配置写入失败', '确认配置目录可写、磁盘有空间，并关闭占用配置的程序，然后点「完成」重试。'),
    'autosave_failed': ('后台保存未完成，程序会继续重试', '检查磁盘空间和配置目录写入权限。保存成功前请保持程序运行。'),
    'target_waiting': ('等待目标窗口，遮盖暂停显示', '打开或还原目标窗口。有多个窗口时，从面板顶部的应用菜单选择。'),
    'tracking_occluded': ('参考区域被遮挡，暂停识别', '移开挡住参考区域的窗口或输入法候选框。需要立即检查时，点「重新定位」。'),
    'tracking_not_found': ('本次未找到可靠的参考区域', '检查参考内容是否变化。先尝试「重新定位」；参考内容已变化时再重新框选。'),
    'tracking_ambiguous': ('有多个相似参考区域', '重新框选一段位置稳定且足够独特的文字或图案。'),
    'tracking_outside': ('遮盖按当前偏移位于窗口外', '在「位置」页减小偏移，或放大目标窗口。'),
    'tracking_manual': ('参考需要重新校准', '在「位置」页重新框选，或改回窗口锚定。'),
    'tracking_unavailable': ('识别暂不可用', '还原目标窗口并保持参考区域无遮挡，点「重新定位」。仍失败时查看同一时间的识别记录。'),
    'render_failed': ('覆盖绘制失败', '先缩小覆盖区域或暂时关闭发光，再试。如果仍失败，导出问题记录。'),
    'pick_failed': ('本次未能取色', '确认目标窗口可见且采样点未被遮挡，再重新取色。'),
    'editor_transport_failed': ('编辑器和主程序的连接断开', '从托盘重新打开编辑面板。核对最后一次编辑是否保存。'),
    'editor_start_failed': ('编辑器未能启动', '退出程序后，从完整解压的当前版目录重新运行。'),
    'startup_failed': ('程序未能启动', '检查是否完整解压当前版。保留配置文件，重试后仍失败时查看本地诊断目录。'),
    'configuration_invalid': ('配置文件格式无法读取', '保留原配置，先复制一份备份，再恢复上次可用的配置文件。'),
    'update_running': ('当前版仍在运行，未替换', '从托盘正常退出当前版，然后重试更新。'),
    'update_prepare_failed': ('新版准备失败，当前版未替换', '检查磁盘空间，或重新获取完整更新包。'),
    'update_archive_failed': ('运行目录已更新，压缩包生成失败', '可继续使用运行目录。检查目标位置的磁盘空间后重新生成压缩包。'),
    'update_verification_failed': ('暂存版本未通过检查，当前版未替换', '保留当前版，重新获取完整更新包再试。'),
    'update_switch_failed': ('更新目录切换失败', '关闭正在使用当前目录的窗口或程序，然后重试。'),
    'update_restored': ('已恢复更新前的版本', '可继续使用当前目录中的程序。'),
    'update_restore_failed': ('更新失败，未能自动恢复', '保留当前目录和已有的 .previous 目录，关闭相关程序后重试更新或导出记录。'),
    'update_cleanup_failed': ('新版已就绪，旧目录暂时无法清理', '先继续使用新版。下次更新前检查 .previous 目录是否被占用。'),
    'unknown_operation_error': ('操作未完成', '检查当前设置后重试。若反复出现，请导出问题记录。'),
}

SOURCES = {'editor': '编辑面板', 'native': '后台程序', 'update': '更新工具'}
LIMIT = 128 * 1024


def describe_issue(code):
    if code in ISSUES:
        return ISSUES[code]
    if code in VISUAL_ERRORS:
        return '操作未完成', VISUAL_ERRORS[code]
    return ISSUES['unknown_operation_error']


class IssueLog:
    def __init__(self, directory):
        self.directory = Path(directory)

    def write(self, code, operation='', cancelled_operations=()):
        code = code if code in ISSUES or code in VISUAL_ERRORS else 'unknown_operation_error'
        record = dict(time=datetime.now(timezone.utc).isoformat(timespec='seconds'),
                      source='editor', code=code, state='failed')
        if operation in OPERATION_LABELS:
            record['operation'] = operation
        if cancelled_operations:
            record['cancelled'] = len(cancelled_operations)
            record['cancelled_operations'] = list(dict.fromkeys(
                op for op in cancelled_operations if op in OPERATION_LABELS))
        try:
            self.directory.mkdir(parents=True, exist_ok=True)
            path = self.directory / 'editor.jsonl'
            if path.exists() and path.stat().st_size >= LIMIT:
                path.replace(path.with_suffix('.previous.jsonl'))
            with path.open('ab') as stream:
                stream.write((json.dumps(record, ensure_ascii=False) + '\n').encode('utf-8'))
        except OSError:
            pass  # Diagnostics cannot block editing or saving.

    def recent(self):
        records = []
        for source in SOURCES:
            for suffix in ('.previous.jsonl', '.jsonl'):
                path = self.directory / (source + suffix)
                try:
                    with path.open('rb') as stream:
                        stream.seek(max(0, path.stat().st_size - LIMIT))
                        lines = stream.read(LIMIT).splitlines()
                except OSError:
                    continue
                for line in lines:
                    try:
                        record = json.loads(line.decode('utf-8-sig'))
                        code = record['code']
                        time = datetime.fromisoformat(record['time']).astimezone()
                        if code not in ISSUES and code not in VISUAL_ERRORS:
                            continue
                        operation = OPERATION_LABELS.get(record.get('operation'), '')
                        cancelled = record.get('cancelled', 0)
                        cancelled = cancelled if type(cancelled) is int and 0 < cancelled < 100000 else 0
                        cancelled_ops = record.get('cancelled_operations', [])
                        cancelled_names = '、'.join(OPERATION_LABELS[op] for op in cancelled_ops
                                                    if isinstance(op, str) and op in OPERATION_LABELS)
                        region = record.get('region', 0)
                        region = region if type(region) is int and 0 < region < 100000 else 0
                        records.append((time, source, code, record.get('state') == 'recovered',
                                        operation, cancelled, cancelled_names, region))
                    except (KeyError, ValueError, TypeError, AttributeError, OverflowError):
                        continue
        return sorted(records, key=lambda item: item[0], reverse=True)[:100]

    def report(self):
        entries = []
        for time, source, code, recovered, operation, cancelled, cancelled_names, region in self.recent():
            title, advice = describe_issue(code)
            header = f'{time:%Y-%m-%d %H:%M:%S}  {SOURCES[source]}  [{code}]'
            detail = ('已恢复。此前问题：' + title) if recovered and code != 'update_restored' else title
            if operation:
                detail = f'{operation} · {detail}'
            if region:
                detail = f'区域 {region} · {detail}'
            if cancelled:
                names = f'（{cancelled_names}）' if cancelled_names else ''
                detail += f'。另有 {cancelled} 项未提交{names}，请检查后重做。'
            entries.append(header + '\n' + detail + ('' if recovered else '\n下一步  ' + advice))
        return '\n\n'.join(entries) or '暂无问题记录。'
