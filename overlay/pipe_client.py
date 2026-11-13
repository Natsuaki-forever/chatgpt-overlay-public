"""Bounded, cancellable Windows message-pipe transport; contains no Qt dependencies."""
import ctypes as C
from ctypes import wintypes as W
import json
import threading
import time

K = C.WinDLL('kernel32', use_last_error=True)


class Overlapped(C.Structure):
    _fields_ = [('internal', C.c_size_t), ('internal_high', C.c_size_t),
        ('offset', W.DWORD), ('offset_high', W.DWORD), ('event', W.HANDLE)]


def bind(name, result, *args):
    function = getattr(K, name)
    function.restype, function.argtypes = result, args
    return function


bind('CreateFileW', W.HANDLE, W.LPCWSTR, W.DWORD, W.DWORD, C.c_void_p, W.DWORD, W.DWORD, W.HANDLE)
bind('CloseHandle', W.BOOL, W.HANDLE)
bind('CreateEventW', W.HANDLE, C.c_void_p, W.BOOL, W.BOOL, W.LPCWSTR)
bind('SetEvent', W.BOOL, W.HANDLE)
bind('ResetEvent', W.BOOL, W.HANDLE)
bind('WaitNamedPipeW', W.BOOL, W.LPCWSTR, W.DWORD)
bind('SetNamedPipeHandleState', W.BOOL, W.HANDLE, C.POINTER(W.DWORD), C.c_void_p, C.c_void_p)
bind('ReadFile', W.BOOL, W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.POINTER(Overlapped))
bind('WriteFile', W.BOOL, W.HANDLE, C.c_void_p, W.DWORD, C.POINTER(W.DWORD), C.POINTER(Overlapped))
bind('GetOverlappedResult', W.BOOL, W.HANDLE, C.POINTER(Overlapped), C.POINTER(W.DWORD), W.BOOL)
bind('CancelIoEx', W.BOOL, W.HANDLE, C.POINTER(Overlapped))
bind('WaitForMultipleObjects', W.DWORD, W.DWORD, C.POINTER(W.HANDLE), W.BOOL, W.DWORD)
bind('WaitForSingleObject', W.DWORD, W.HANDLE, W.DWORD)


class PipeClient:
    message_limit = 1024*1024

    def __init__(self, name, timeout_ms=5000):
        self._io_lock = threading.RLock()
        self._handle_lock = threading.Lock()
        self.handle = None
        self.stop = self.event = None
        self.timeout_ms = timeout_ms
        try:
            deadline = time.monotonic()+timeout_ms/1000
            while True:
                handle = K.CreateFileW(name, 0xC0000000, 0, None, 3, 0x40000000 | 0x110000, None)
                if handle != C.c_void_p(-1).value:
                    self.handle = handle
                    break
                error = C.get_last_error()
                if error not in (2, 231) or time.monotonic() >= deadline:
                    raise C.WinError(error)
                K.WaitNamedPipeW(name, 50)
                time.sleep(.01)
            mode = W.DWORD(2)
            if not K.SetNamedPipeHandleState(self.handle, C.byref(mode), None, None):
                raise C.WinError(C.get_last_error())
            self.stop = K.CreateEventW(None, True, False, None)
            self.event = K.CreateEventW(None, True, False, None)
            if not self.stop or not self.event:
                raise C.WinError(C.get_last_error())
        except BaseException:
            self.close()
            raise

    def cancel(self):
        with self._handle_lock:
            if self.stop:
                K.SetEvent(self.stop)

    def close(self):
        self.cancel()
        # The operation observes stop, cancels its OVERLAPPED and waits for completion
        # before this lock can be acquired and any referenced HANDLE is closed.
        with self._io_lock, self._handle_lock:
            for handle in (self.handle, self.stop, self.event):
                if handle:
                    K.CloseHandle(handle)
            self.handle = self.stop = self.event = None

    def _io(self, begin):
        if not self.handle or K.WaitForSingleObject(self.stop, 0) == 0:
            raise OSError(995, 'Local editor session cancelled')
        operation = Overlapped()
        operation.event = self.event
        K.ResetEvent(self.event)
        count = W.DWORD()
        if begin(C.byref(operation), C.byref(count)):
            return 0, count.value
        error = C.get_last_error()
        if error == 234:
            K.GetOverlappedResult(self.handle, C.byref(operation), C.byref(count), False)
            return 234, count.value
        if error != 997:
            return error, count.value
        handles = (W.HANDLE*2)(self.stop, self.event)
        result = K.WaitForMultipleObjects(2, handles, False, self.timeout_ms)
        if result != 1:
            K.CancelIoEx(self.handle, C.byref(operation))
            K.GetOverlappedResult(self.handle, C.byref(operation), C.byref(count), True)
            if result == 258:
                raise TimeoutError('Local editor session response timed out')
            raise OSError(995, 'Local editor session cancelled')
        if K.GetOverlappedResult(self.handle, C.byref(operation), C.byref(count), False):
            return 0, count.value
        return C.get_last_error(), count.value

    def write(self, data):
        with self._io_lock:
            self._write(data)

    def _write(self, data):
        buffer = C.create_string_buffer(data)
        error, written = self._io(lambda operation, count: K.WriteFile(self.handle, buffer, len(data), count, operation))
        if error:
            raise C.WinError(error)
        if written != len(data):
            raise OSError('Incomplete local session write')

    def read(self):
        with self._io_lock:
            return self._read()

    def _read(self):
        chunks, size = [], 0
        while True:
            buffer = C.create_string_buffer(65536)
            error, count = self._io(lambda operation, read: K.ReadFile(self.handle, buffer, len(buffer), read, operation))
            if error not in (0, 234):
                raise C.WinError(error)
            chunks.append(buffer.raw[:count])
            size += count
            if size > self.message_limit:
                raise ValueError('Local session response exceeds the message limit')
            if not error:
                result = json.loads(b''.join(chunks).decode('utf-8'))
                if not isinstance(result, dict):
                    raise ValueError('Local session response must be an object')
                return result

    def exchange(self, request):
        data = json.dumps(request, ensure_ascii=False, allow_nan=False).encode('utf-8')
        if len(data) > self.message_limit:
            raise ValueError('Local session request exceeds the message limit')
        with self._io_lock:
            self.write(data)
            return self.read()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
