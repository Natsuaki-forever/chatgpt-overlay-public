"""Windows process memory counters without psutil or Qt."""
import ctypes as C
from ctypes import wintypes as W


def memory():
    class Counters(C.Structure):
        _fields_ = [('cb', W.DWORD), ('faults', W.DWORD)] + [
            (key, C.c_size_t) for key in ('peak_wset', 'wset', 'peak_paged', 'paged',
                                         'peak_nonpaged', 'nonpaged', 'pagefile',
                                         'peak_pagefile', 'private')]
    k, p = C.WinDLL('kernel32'), C.WinDLL('psapi', use_last_error=True)
    k.GetCurrentProcess.restype = W.HANDLE
    p.GetProcessMemoryInfo.argtypes = [W.HANDLE, C.POINTER(Counters), W.DWORD]
    p.GetProcessMemoryInfo.restype = W.BOOL
    counters = Counters(cb=C.sizeof(Counters))
    if not p.GetProcessMemoryInfo(k.GetCurrentProcess(), C.byref(counters), counters.cb):
        raise C.WinError(C.get_last_error())
    result = {name+'_mib': round(getattr(counters, name)/1048576, 3)
              for name in ('wset', 'peak_wset', 'private')}
    times = [W.FILETIME() for _ in range(4)]
    k.GetProcessTimes.argtypes = [W.HANDLE]+[C.POINTER(W.FILETIME)]*4
    k.GetProcessTimes.restype = W.BOOL
    if not k.GetProcessTimes(k.GetCurrentProcess(), *(C.byref(t) for t in times)):
        raise C.WinError(C.get_last_error())
    result['cpu_seconds'] = sum((t.dwHighDateTime << 32)+t.dwLowDateTime for t in times[2:])/10000000
    return result
