"""Typed Win32 boundary. All hooks live on Qt's GUI/message-loop thread."""
import ctypes as C
from ctypes import wintypes as W
from pathlib import PureWindowsPath

u = C.WinDLL("user32", use_last_error=True)
k = C.WinDLL("kernel32", use_last_error=True)
dwm = C.WinDLL("dwmapi")
PTR = C.c_ssize_t
CALLBACK = C.WINFUNCTYPE(None, W.HANDLE, W.DWORD, W.HWND, W.LONG, W.LONG, W.DWORD, W.DWORD)
ENUM = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)


def bind(lib, name, result, *args):
    fn = getattr(lib, name)
    fn.restype, fn.argtypes = result, list(args)
    return fn


bind(u, "GetForegroundWindow", W.HWND)
bind(u, "GetWindowThreadProcessId", W.DWORD, W.HWND, C.POINTER(W.DWORD))
bind(u, "IsWindow", W.BOOL, W.HWND)
bind(u, "IsWindowVisible", W.BOOL, W.HWND)
bind(u, "IsIconic", W.BOOL, W.HWND)
bind(u, "GetClientRect", W.BOOL, W.HWND, C.POINTER(W.RECT))
bind(u, "ClientToScreen", W.BOOL, W.HWND, C.POINTER(W.POINT))
bind(u, "GetDpiForWindow", W.UINT, W.HWND)
bind(u, "MonitorFromWindow", W.HANDLE, W.HWND, W.DWORD)


class MONITORINFO(C.Structure):
    _fields_ = [('cbSize', W.DWORD), ('rcMonitor', W.RECT), ('rcWork', W.RECT), ('dwFlags', W.DWORD)]


class MONITORINFOEX(C.Structure):
    """MONITORINFO plus the Windows display device identifier."""
    _fields_ = MONITORINFO._fields_ + [('szDevice', W.WCHAR * 32)]


# This API accepts MONITORINFO or its larger MONITORINFOEX form.
bind(u, "GetMonitorInfoW", W.BOOL, W.HANDLE, C.c_void_p)
bind(u, "EnumWindows", W.BOOL, ENUM, W.LPARAM)
bind(u, "GetWindow", W.HWND, W.HWND, W.UINT)
bind(u, "GetWindowLongPtrW", PTR, W.HWND, C.c_int)
bind(u, "SetWindowLongPtrW", PTR, W.HWND, C.c_int, PTR)
bind(u, "SetWindowPos", W.BOOL, W.HWND, W.HWND, C.c_int, C.c_int, C.c_int, C.c_int, W.UINT)
bind(u, "ShowWindow", W.BOOL, W.HWND, C.c_int)
bind(u, "SetWinEventHook", W.HANDLE, W.DWORD, W.DWORD, W.HMODULE, CALLBACK, W.DWORD, W.DWORD, W.DWORD)
bind(u, "UnhookWinEvent", W.BOOL, W.HANDLE)
bind(u, "RegisterHotKey", W.BOOL, W.HWND, C.c_int, W.UINT, W.UINT)
bind(u, "UnregisterHotKey", W.BOOL, W.HWND, C.c_int)
bind(u, "SetForegroundWindow", W.BOOL, W.HWND)
bind(u, "AllowSetForegroundWindow", W.BOOL, W.DWORD)
bind(u, "MessageBoxW", C.c_int, W.HWND, W.LPCWSTR, W.LPCWSTR, W.UINT)
bind(k, "OpenProcess", W.HANDLE, W.DWORD, W.BOOL, W.DWORD)
bind(k, "QueryFullProcessImageNameW", W.BOOL, W.HANDLE, W.DWORD, W.LPWSTR, C.POINTER(W.DWORD))
bind(k, "CloseHandle", W.BOOL, W.HANDLE)
bind(dwm, "DwmGetWindowAttribute", C.c_long, W.HWND, W.DWORD, C.c_void_p, W.DWORD)
bind(dwm, "DwmSetWindowAttribute", C.c_long, W.HWND, W.DWORD, C.c_void_p, W.DWORD)


def dark_frame(hwnd):
    """Windows 11 frame colors and subtle system-drawn window corners."""
    # COLORREF is 0x00BBGGRR; caption matches the editor's #202427 surface.
    # DWMWA_WINDOW_CORNER_PREFERENCE=33, DWMWCP_ROUNDSMALL=3.
    results = []
    for attribute, value in ((20, 1), (33, 3), (34, 0x3D3830), (35, 0x272420), (36, 0xF2F0ED)):
        color = W.DWORD(value)
        results.append(dwm.DwmSetWindowAttribute(hwnd, attribute, C.byref(color), C.sizeof(color)))
    return results


def enable_dpi():
    # Must run before QApplication. PyInstaller manifest also declares PMv2.
    bind(u, "SetProcessDpiAwarenessContext", W.BOOL, C.c_void_p)(C.c_void_p(-4))


def pid(hwnd):
    value = W.DWORD()
    u.GetWindowThreadProcessId(hwnd, C.byref(value))
    return value.value


def executable(hwnd):
    handle = k.OpenProcess(0x1000, False, pid(hwnd))
    if not handle:
        return ""
    try:
        size = W.DWORD(32768)
        buffer = C.create_unicode_buffer(size.value)
        return buffer.value if k.QueryFullProcessImageNameW(handle, 0, buffer, C.byref(size)) else ""
    finally:
        k.CloseHandle(handle)


def available(hwnd):
    if not hwnd or not u.IsWindow(hwnd) or not u.IsWindowVisible(hwnd) or u.IsIconic(hwnd):
        return False
    cloaked = W.DWORD()
    hr = dwm.DwmGetWindowAttribute(hwnd, 14, C.byref(cloaked), C.sizeof(cloaked))
    return hr == 0 and not cloaked.value


def find_target():
    candidates = targets()
    foreground = u.GetForegroundWindow()
    return foreground if foreground in candidates else (candidates[0] if candidates else None)


def is_target(hwnd):
    return bool(hwnd) and PureWindowsPath(executable(hwnd)).name.lower() == "chatgpt.exe"


def targets():
    """Return every visible top-level ChatGPT window, including a second one."""
    candidates = []
    @ENUM
    def visit(hwnd, _):
        if available(hwnd) and not u.GetWindow(hwnd, 4) and is_target(hwnd):
            candidates.append(hwnd)
        return True
    u.EnumWindows(visit, 0)
    return candidates


def client_box(hwnd):
    rect, point = W.RECT(), W.POINT()
    if not u.GetClientRect(hwnd, C.byref(rect)) or not u.ClientToScreen(hwnd, C.byref(point)):
        return None
    return point.x, point.y, rect.right, rect.bottom


def monitor_box(hwnd):
    """Physical monitor/work-area rectangles, including negative origins."""
    monitor = u.MonitorFromWindow(hwnd, 2)  # MONITOR_DEFAULTTONEAREST
    info = MONITORINFO()
    info.cbSize = C.sizeof(info)
    if not monitor or not u.GetMonitorInfoW(monitor, C.byref(info)):
        return None
    def box(r):
        return r.left, r.top, r.right-r.left, r.bottom-r.top
    return box(info.rcMonitor), box(info.rcWork)


def monitor_device(hwnd):
    """Return e.g. ``\\\\.\\DISPLAY1`` for the monitor holding hwnd."""
    monitor = u.MonitorFromWindow(hwnd, 2)
    info = MONITORINFOEX()
    info.cbSize = C.sizeof(info)
    if not monitor or not u.GetMonitorInfoW(monitor, C.byref(info)):
        return ""
    return info.szDevice


def configure(hwnd, owner, editing):
    if u.GetWindowLongPtrW(hwnd, -8) != (owner or 0):
        u.SetWindowLongPtrW(hwnd, -8, owner or 0)  # external owner, no injection
    style = u.GetWindowLongPtrW(hwnd, -20)
    style |= 0x80000 | 0x80  # layered, toolwindow
    if editing:
        style &= ~(0x20 | 0x08000000)
    else:
        style |= 0x20 | 0x08000000  # transparent + noactivate
    if u.GetWindowLongPtrW(hwnd, -20) != style:
        u.SetWindowLongPtrW(hwnd, -20, style)
        u.SetWindowPos(hwnd, 0, 0, 0, 0, 0, 0x0010 | 0x0001 | 0x0002 | 0x0004 | 0x0020)


def place(hwnd, box, restack=True):
    # Insert immediately above owner, preserving unrelated windows above it.
    owner = u.GetWindow(hwnd, 4)
    previous = u.GetWindow(owner, 3) if owner else None  # GW_HWNDPREV
    # Inserting after a topmost HWND can promote us to the topmost band.
    # HWND_TOP means the top of our existing non-topmost band instead.
    if previous and previous != hwnd and u.GetWindowLongPtrW(previous, -20) & 0x8:
        previous = None
    flags = 0x0010 | 0x0200
    if previous == hwnd or not restack:
        flags |= 0x0004  # already in place; SWP_NOZORDER
        previous = None
        if client_box(hwnd) == box:
            return True  # No geometry/z-order change: avoid another native event.
    return u.SetWindowPos(hwnd, previous or 0, *box, flags)


class Hooks:
    def __init__(self, callback):
        self.callback = CALLBACK(callback)
        self.handles = []
        for low, high in ((3, 3), (0x16, 0x17), (0x8000, 0x8003), (0x800B, 0x800C)):
            handle = u.SetWinEventHook(low, high, None, self.callback, 0, 0, 0)
            if not handle:
                self.close()
                raise C.WinError(C.get_last_error())
            self.handles.append(handle)

    def close(self):
        for handle in self.handles:
            u.UnhookWinEvent(handle)
        self.handles.clear()
