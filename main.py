import argparse
import ctypes
import sys
import os


def main():
    if sys.platform != "win32":
        raise SystemExit("ChatGPT Overlay 仅支持 Windows 11。")
    from overlay import native
    native.enable_dpi()
    parser = argparse.ArgumentParser(description="ChatGPT Overlay")
    parser.add_argument("--demo", action="store_true", help="使用本地演示窗口")
    parser.add_argument("--check", action="store_true", help="初始化并在 1.5 秒后退出，用于验证打包")
    parser.add_argument('--check-seconds', type=float, default=1.5, help='诊断运行时长（需配合 --check）')
    parser.add_argument('--config', help='指定独立配置文件')
    parser.add_argument('--target-hwnd', type=lambda value: int(value, 0), help=argparse.SUPPRESS)
    parser.add_argument('--sample-file', help=argparse.SUPPRESS)
    parser.add_argument('--ready-file', help=argparse.SUPPRESS)
    args = parser.parse_args()
    if args.check_seconds <= 0 or not args.check_seconds < 86400:
        parser.error('--check-seconds must be between 0 and 86400')
    # Prevent duplicate tray apps, global hotkeys and conflicting config writers.
    native.bind(native.k, "CreateMutexW", native.W.HANDLE, ctypes.c_void_p, native.W.BOOL, native.W.LPCWSTR)
    mutex_name = f"Local\\ChatGPTOverlay.Check.{os.getpid()}" if args.check else "Local\\ChatGPTOverlay.SingleInstance"
    mutex = native.k.CreateMutexW(None, False, mutex_name)
    if not mutex:
        raise ctypes.WinError(ctypes.get_last_error())
    if ctypes.get_last_error() == 183:
        native.k.CloseHandle(mutex)
        native.u.MessageBoxW(None, "工具已在运行，请使用系统托盘图标。", "ChatGPT Overlay", 0x40)
        return 0
    from PySide6.QtWidgets import QApplication, QLabel, QVBoxLayout, QWidget
    from PySide6.QtCore import Qt, QTimer
    from overlay.ui import Controller
    from overlay.model import Store
    app = QApplication(sys.argv)
    app.setApplicationName("ChatGPT Overlay")
    app.setQuitOnLastWindowClosed(False)
    demo = None
    if args.demo:
        demo = QWidget()
        demo.setWindowTitle("ChatGPT · Overlay 演示")
        demo.resize(960, 660)
        demo.setStyleSheet("background:#212121;color:#ececec;font:16px 'Segoe UI';")
        layout = QVBoxLayout(demo)
        layout.addWidget(QLabel("ChatGPT"))
        center = QLabel("按 Ctrl+Shift+E，拖动框选区域。\n\n可移动、缩放此窗口验证覆盖跟随。")
        center.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(center, 1)
        demo.show()
    if args.target_hwnd:
        if not args.check or not native.u.IsWindow(args.target_hwnd):
            parser.error('--target-hwnd requires --check and a valid test window')
        from types import SimpleNamespace
        demo = SimpleNamespace(winId=lambda: args.target_hwnd)
    try:
        controller = Controller(app, store=Store(args.config) if args.config else None, demo=demo, register_hotkey=not args.check)
        if args.sample_file or args.ready_file:
            from overlay.diagnostics import Recorder
            recorder = Recorder(app, controller, args.sample_file, args.ready_file)
        if args.check:
            QTimer.singleShot(round(args.check_seconds*1000), app.quit)
        code = app.exec()
    finally:
        native.k.CloseHandle(mutex)
    return code


if __name__ == "__main__":
    try:
        result = main()
    except Exception:
        import os
        import traceback
        from pathlib import Path
        details = traceback.format_exc()
        log = Path.cwd() / "startup-error.log" if "--check" in sys.argv else Path(os.environ.get("APPDATA", str(Path.home()))) / "ChatGPTOverlay" / "startup-error.log"
        try:
            log.parent.mkdir(parents=True, exist_ok=True)
            log.write_text(details, encoding="utf-8")
        except OSError:
            pass
        if "--check" not in sys.argv:
            ctypes.windll.user32.MessageBoxW(None, f"启动失败。错误记录：{log}\n\n{details[-1500:]}", "ChatGPT Overlay", 0x10)
        result = 1
    sys.exit(result)
