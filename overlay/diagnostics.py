"""Opt-in diagnostics. Never records region text or changes a user setting."""
import ctypes as C
from ctypes import wintypes as W
import json
import os
from pathlib import Path
import time


class Memory(C.Structure):
    _fields_ = [('cb', W.DWORD), ('faults', W.DWORD)] + [(name, C.c_size_t) for name in
        ('peak_working', 'working', 'peak_paged', 'paged', 'peak_nonpaged', 'nonpaged',
         'pagefile', 'peak_pagefile', 'private')]


class ProcessEntry(C.Structure):
    _fields_ = [('size', W.DWORD), ('usage', W.DWORD), ('pid', W.DWORD),
        ('heap', C.c_size_t), ('module', W.DWORD), ('threads', W.DWORD),
        ('parent', W.DWORD), ('priority', W.LONG), ('flags', W.DWORD), ('exe', W.WCHAR*260)]


def thread_count(pid):
    from . import native
    native.bind(native.k, 'CreateToolhelp32Snapshot', W.HANDLE, W.DWORD, W.DWORD)
    native.bind(native.k, 'Process32FirstW', W.BOOL, W.HANDLE, C.POINTER(ProcessEntry))
    native.bind(native.k, 'Process32NextW', W.BOOL, W.HANDLE, C.POINTER(ProcessEntry))
    snapshot = native.k.CreateToolhelp32Snapshot(2, 0)
    if snapshot == C.c_void_p(-1).value:
        return None
    try:
        entry = ProcessEntry()
        entry.size = C.sizeof(entry)
        valid = native.k.Process32FirstW(snapshot, C.byref(entry))
        while valid:
            if entry.pid == pid:
                return entry.threads
            valid = native.k.Process32NextW(snapshot, C.byref(entry))
    finally:
        native.k.CloseHandle(snapshot)
    return None


def process_sample(pid=None):
    from . import native
    psapi = C.WinDLL('psapi')
    native.bind(psapi, 'GetProcessMemoryInfo', W.BOOL, W.HANDLE, C.POINTER(Memory), W.DWORD)
    native.bind(native.k, 'GetCurrentProcess', W.HANDLE)
    native.bind(native.k, 'GetProcessHandleCount', W.BOOL, W.HANDLE, C.POINTER(W.DWORD))
    native.bind(native.k, 'GetProcessTimes', W.BOOL, W.HANDLE, *([C.POINTER(W.FILETIME)]*4))
    native.bind(native.u, 'GetGuiResources', W.DWORD, W.HANDLE, W.DWORD)
    external = pid is not None and pid != os.getpid()
    handle = native.k.OpenProcess(0x410, False, pid) if external else native.k.GetCurrentProcess()
    if not handle:
        raise C.WinError(C.get_last_error())
    memory = Memory()
    memory.cb = C.sizeof(memory)
    if not psapi.GetProcessMemoryInfo(handle, C.byref(memory), memory.cb):
        if external:
            native.k.CloseHandle(handle)
        raise C.WinError(C.get_last_error())
    handles = W.DWORD()
    native.k.GetProcessHandleCount(handle, C.byref(handles))
    created, exited, kernel, user = (W.FILETIME() for _ in range(4))
    native.k.GetProcessTimes(handle, *map(C.byref, (created, exited, kernel, user)))
    seconds = lambda ft: ((ft.dwHighDateTime << 32) | ft.dwLowDateTime)/10000000
    result = dict(pid=pid or os.getpid(), wall_time=time.time(), working=memory.working,
                private=memory.private, cpu_seconds=seconds(kernel)+seconds(user),
                handles=handles.value, gdi=native.u.GetGuiResources(handle, 0),
                user_objects=native.u.GetGuiResources(handle, 1))
    if external:
        native.k.CloseHandle(handle)
    result['threads'] = thread_count(pid or os.getpid())
    return result


class Recorder:
    def __init__(self, app, controller, sample_file=None, ready_file=None):
        from PySide6.QtCore import QTimer
        self.controller, self.sample_file = controller, Path(sample_file) if sample_file else None
        if self.sample_file:
            self.sample_file.parent.mkdir(parents=True, exist_ok=True)
            self.sample_file.write_text('', encoding='utf-8')
        self.timer = QTimer(controller)
        self.timer.setInterval(1000)
        self.timer.timeout.connect(self.sample)
        self.timer.start()
        self.sample()
        def ready():
            controller.sync()
            controller.canvas.repaint()
            for window in controller.surfaces.windows:
                window.repaint()
            if ready_file:
                path = Path(ready_file)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(dict(pid=os.getpid(), wall_time=time.time(),
                    first_frame_visible=controller.canvas.isVisible() or controller.surfaces.visible(),
                    hotkey_registered=controller.hotkey, tray_visible=controller.tray.isVisible())), encoding='utf-8')
        QTimer.singleShot(0, ready)
        app.aboutToQuit.connect(self.finish)

    def finish(self):
        self.timer.stop()
        self.sample(phase='shutdown')

    def sample(self, phase='running'):
        if self.sample_file:
            row = process_sample()
            row['phase'] = phase
            row['editing'] = self.controller.editing
            row['regions'] = len(self.controller.regions)
            row['canvas_pixels'] = round(self.controller.canvas.width()*self.controller.canvas.height()*self.controller.canvas.devicePixelRatioF()**2) if self.controller.canvas.isVisible() else 0
            row['canvas_pixels'] += self.controller.surfaces.physical_pixels
            row['surface_count'] = len(self.controller.surfaces.windows)
            row['frames'] = self.controller.canvas.frames + sum(window.frames for window in self.controller.surfaces.windows)
            with self.sample_file.open('a', encoding='utf-8') as stream:
                stream.write(json.dumps(row)+'\n')
