"""Exercise the real packaged editor against an owned target and isolated config."""
import argparse
import ctypes as C
import json
from pathlib import Path
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from overlay import native
native.enable_dpi()
from PySide6.QtWidgets import QApplication,QWidget
from PySide6.QtTest import QTest

parser=argparse.ArgumentParser();parser.add_argument('--exe',type=Path,required=True)
parser.add_argument('--output',type=Path,required=True);args=parser.parse_args()
out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
app=QApplication([]);app.setQuitOnLastWindowClosed(False)
target=QWidget();target.resize(800,500);target.setWindowTitle('Overlay editor entry fixture');target.show()
config=out/'config.json';config.write_text(json.dumps(dict(version=1,regions=[dict(text='ENTRY FIXTURE')])),'utf-8')
ready=out/'ready.json';samples=out/'samples.jsonl'
native.bind(native.u,'PostMessageW',native.W.BOOL,native.W.HWND,native.W.UINT,native.W.WPARAM,native.W.LPARAM)
process=subprocess.Popen([str(args.exe.resolve()),'--check','--check-seconds','25','--editor-host',
    '--target-hwnd',str(int(target.winId())),'--config',str(config),'--ready-file',str(ready),'--sample-file',str(samples)])
latest={};report=dict(passed=False,real_chatgpt_verified=False)
def state():
    global latest
    if samples.exists():
        for line in reversed(samples.read_text().splitlines()):
            try:latest=json.loads(line);break
            except json.JSONDecodeError:pass
    return latest
def wait(predicate,seconds=8):
    end=time.monotonic()+seconds
    while not predicate():
        assert process.poll() is None,('engine exited',process.returncode)
        assert time.monotonic()<end,latest
        QTest.qWait(30)
def editor_window(pid):
    windows=[]
    @native.ENUM
    def visit(hwnd,unused):
        actual=native.W.DWORD();native.u.GetWindowThreadProcessId(hwnd,C.byref(actual))
        if actual.value==pid and native.u.IsWindowVisible(hwnd):windows.append(int(hwnd))
        return True
    native.u.EnumWindows(visit,0)
    return windows[0] if windows else None
try:
    wait(ready.exists);marker=json.loads(ready.read_text())
    pid=marker['editor_pid'];host=marker['host_hwnd']
    wait(lambda:editor_window(pid));panel=editor_window(pid)
    native.u.PostMessageW(host,0x8000+56,0,0);native.u.PostMessageW(host,0x8000+56,0,0)
    wait(lambda:state().get('editing') and latest.get('editor_pid')==pid and editor_window(pid))
    report['repeated_open_keeps_editing']=True
    native.u.ShowWindow(panel,6);QTest.qWait(80);assert native.u.IsIconic(panel)
    native.u.PostMessageW(host,0x8000+56,0,0)
    wait(lambda:not native.u.IsIconic(panel))
    assert native.u.IsWindowVisible(panel)
    report['minimized_panel_restores']=True
    target.close();target.destroy()
    QTest.qWait(700)
    assert native.u.IsWindowVisible(panel) and state()['editing']
    report['target_close_keeps_waiting_editor']=True
    native.u.PostMessageW(panel,0x0010,0,0)
    wait(lambda:not state().get('editing',True) and state().get('editor_pid')==0)
    assert json.loads(config.read_text())['regions'][0]['text']=='ENTRY FIXTURE'
    report.update(close_saves_and_exits_editor=True,passed=True)
finally:
    report['last_state']=latest
    (out/'result.json').write_text(json.dumps(report,indent=2),'utf-8')
    if process.poll() is None:
        if 'host' in locals():native.u.PostMessageW(host,0x8000+55,0,0)
        try:process.wait(timeout=5)
        except subprocess.TimeoutExpired:process.terminate();process.wait(timeout=5)
    target.close()
