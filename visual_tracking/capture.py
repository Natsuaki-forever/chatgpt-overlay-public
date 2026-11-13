"""Bounded Win32 diagnostic capture. No hooks, window changes or Qt imports."""
import ctypes as C
from ctypes import wintypes as W

u = C.WinDLL('user32', use_last_error=True)
g = C.WinDLL('gdi32', use_last_error=True)
dwm = C.WinDLL('dwmapi')


def bind(lib, name, result, *args):
    fn = getattr(lib, name)
    fn.restype, fn.argtypes = result, args
    return fn


PTR = C.c_void_p
bind(u, 'GetForegroundWindow', W.HWND)
bind(u, 'GetAncestor', W.HWND, W.HWND, W.UINT)
bind(u, 'IsWindow', W.BOOL, W.HWND)
bind(u, 'IsWindowVisible', W.BOOL, W.HWND)
bind(u, 'IsIconic', W.BOOL, W.HWND)
bind(u, 'GetClientRect', W.BOOL, W.HWND, C.POINTER(W.RECT))
bind(u, 'GetWindowRect', W.BOOL, W.HWND, C.POINTER(W.RECT))
bind(u, 'ClientToScreen', W.BOOL, W.HWND, C.POINTER(W.POINT))
bind(u, 'GetDpiForWindow', W.UINT, W.HWND)
bind(u, 'GetDC', W.HDC, W.HWND)
bind(u, 'ReleaseDC', C.c_int, W.HWND, W.HDC)
bind(u, 'GetWindowThreadProcessId', W.DWORD, W.HWND, C.POINTER(W.DWORD))
bind(u, 'SetProcessDpiAwarenessContext', W.BOOL, PTR)
bind(u, 'SetThreadDpiAwarenessContext', PTR, PTR)
bind(g, 'CreateCompatibleDC', W.HDC, W.HDC)
bind(g, 'CreateCompatibleBitmap', W.HBITMAP, W.HDC, C.c_int, C.c_int)
bind(g, 'SelectObject', PTR, W.HDC, PTR)
bind(g, 'DeleteObject', W.BOOL, PTR)
bind(g, 'DeleteDC', W.BOOL, W.HDC)
bind(g, 'BitBlt', W.BOOL, W.HDC, C.c_int, C.c_int, C.c_int, C.c_int, W.HDC, C.c_int, C.c_int, W.DWORD)
bind(dwm, 'DwmGetWindowAttribute', C.c_long, W.HWND, W.DWORD, PTR, W.DWORD)
CALLBACK = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)
bind(u, 'EnumWindows', W.BOOL, CALLBACK, W.LPARAM)


class BI(C.Structure):
    _fields_ = [('size', W.DWORD), ('width', W.LONG), ('height', W.LONG),
                ('planes', W.WORD), ('bits', W.WORD), ('compression', W.DWORD),
                ('image_size', W.DWORD), ('x', W.LONG), ('y', W.LONG),
                ('used', W.DWORD), ('important', W.DWORD)]


bind(g, 'GetDIBits', C.c_int, W.HDC, W.HBITMAP, W.UINT, W.UINT, PTR, C.POINTER(BI), W.UINT)


def dpi_init():
    u.SetProcessDpiAwarenessContext(PTR(-4))
    if not u.SetThreadDpiAwarenessContext(PTR(-4)):
        raise C.WinError(C.get_last_error())


def client(hwnd):
    rect, origin = W.RECT(), W.POINT()
    if not u.GetClientRect(hwnd, C.byref(rect)) or not u.ClientToScreen(hwnd, C.byref(origin)):
        raise C.WinError(C.get_last_error())
    return origin.x, origin.y, rect.right, rect.bottom


def safe_target(hwnd):
    if (not u.IsWindow(hwnd) or not u.IsWindowVisible(hwnd) or u.IsIconic(hwnd) or
            u.GetAncestor(u.GetForegroundWindow(), 2) != hwnd):
        return False
    x, y, w, h = client(hwnd)
    if w <= 0 or h <= 0:
        return False
    blocked = False
    found = False

    @CALLBACK
    def visit(other, unused):
        nonlocal blocked, found
        if other == hwnd:
            found = True
            return False
        if not u.IsWindowVisible(other) or u.IsIconic(other):
            return True
        cloaked = W.DWORD()
        if dwm.DwmGetWindowAttribute(other, 14, C.byref(cloaked), 4) < 0:
            blocked = True
            return False
        if cloaked.value:
            return True
        r = W.RECT()
        if not u.GetWindowRect(other, C.byref(r)):
            blocked = True
            return False
        if min(x+w, r.right) > max(x, r.left) and min(y+h, r.bottom) > max(y, r.top):
            blocked = True
            return False
        return True

    u.EnumWindows(visit, 0)
    return found and not blocked


def grab(rect, pixel_limit=524288):
    import numpy as np
    if len(rect) != 4 or any(type(v) is not int for v in rect):
        raise ValueError('invalid-capture-rect')
    x, y, w, h = rect
    if w < 1 or h < 1 or w*h > pixel_limit:
        raise ValueError('capture-pixel-budget-exceeded')
    dc = u.GetDC(None)
    if not dc:
        raise C.WinError(C.get_last_error())
    mem, bmp, old = None, None, None
    try:
        mem = g.CreateCompatibleDC(dc)
        if not mem:
            raise C.WinError(C.get_last_error())
        bmp = g.CreateCompatibleBitmap(dc, w, h)
        if not bmp:
            raise C.WinError(C.get_last_error())
        old = g.SelectObject(mem, bmp)
        if not old or old == C.c_void_p(-1).value:
            raise C.WinError(C.get_last_error())
        if not g.BitBlt(mem, 0, 0, w, h, dc, x, y, 0x40CC0020):
            raise C.WinError(C.get_last_error())
        g.SelectObject(mem, old)
        info = BI(size=C.sizeof(BI), width=w, height=-h, planes=1, bits=32)
        pixels = np.empty((h, w, 4), np.uint8)
        if g.GetDIBits(mem, bmp, 0, h, pixels.ctypes.data, C.byref(info), 0) != h:
            raise RuntimeError('capture-read-failed')
        return pixels[:, :, :3].copy()
    finally:
        if mem and old and old != C.c_void_p(-1).value:
            g.SelectObject(mem, old)
        if bmp:
            g.DeleteObject(bmp)
        if mem:
            g.DeleteDC(mem)
        u.ReleaseDC(None, dc)
