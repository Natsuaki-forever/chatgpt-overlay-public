"""Internal on-demand editor entry; normally launched only by the native host."""
import argparse
import ctypes
import os
import sys


def main():
    parser = argparse.ArgumentParser(description='ChatGPT Overlay internal editor')
    parser.add_argument('--pipe', required=True)
    parser.add_argument('--session', required=True)
    args = parser.parse_args()
    if len(args.session) != 32 or any(char not in '0123456789abcdef' for char in args.session):
        parser.error('Invalid editor session')
    from overlay import native
    native.enable_dpi()
    native.bind(native.k, 'CreateMutexW', native.W.HANDLE, ctypes.c_void_p, native.W.BOOL, native.W.LPCWSTR)
    mutex = native.k.CreateMutexW(None, False, 'Local\\ChatGPTOverlay.Editor.'+args.session)
    if not mutex:
        raise ctypes.WinError(ctypes.get_last_error())
    if ctypes.get_last_error() == 183:
        native.k.CloseHandle(mutex)
        return 1
    try:
        # Limit editor-only font database overhead on machines with large font
        # collections. The native overlay renderer continues using DirectWrite.
        os.environ.setdefault('QT_QPA_PLATFORM','windows:fontengine=gdi')
        from PySide6.QtWidgets import QApplication
        from PySide6.QtGui import QFont
        from overlay.remote_session import RemoteSession
        app = QApplication(sys.argv)
        app.setApplicationName('ChatGPT Overlay Editor')
        app.setQuitOnLastWindowClosed(False)
        from overlay.editor_theme import editor_font
        app.setFont(editor_font())
        session = RemoteSession(app, args.pipe, args.session)
        session.start()
        try:
            result = app.exec()
            if session.error:
                raise RuntimeError(session.error)
            return result
        finally:
            session.worker.stop()
            if not session.worker.wait(6000):
                raise RuntimeError('Editor transport did not stop after cancellation')
    finally:
        native.k.CloseHandle(mutex)


if __name__ == '__main__':
    try:
        result = main()
    except Exception:
        import traceback
        from pathlib import Path
        path = Path(os.environ.get('APPDATA', str(Path.home())))/'ChatGPTOverlay'/'editor-error.log'
        try:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(traceback.format_exc(), encoding='utf-8')
        except OSError:
            pass
        result = 1
    sys.exit(result)
