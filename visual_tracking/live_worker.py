"""Bounded multi-region pipe worker. No Qt, window manipulation or writes."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import os
import sys
import time
from collections import OrderedDict

from .contracts import LIMITS, SINGLE_THREAD_ENV

PROTOCOL = 'visual-worker-1'
permission_diagnostic = {}


def checked_template(value):
    if not isinstance(value, str) or len(value) > 262168:
        raise ValueError('invalid-template')
    data = bytes.fromhex(value)
    if len(data) < 13 or data[:4] != b'VTP1':
        raise ValueError('invalid-template')
    import struct
    w, h = struct.unpack('<II', data[4:12])
    if not w or not h or w*h > LIMITS.template_pixels or len(data) != 12+w*h:
        raise ValueError('invalid-template')
    import numpy as np
    return np.frombuffer(data, dtype=np.uint8, offset=12).reshape(h, w).copy()


def permission(context, positive=False):
    global permission_diagnostic
    permission_diagnostic={}
    from . import capture as cap
    hwnd, expected = context['hwnd'], tuple(context['client'])
    def deny(reason, **values):
        permission_diagnostic.update(reason=reason,**values)
        return False
    if not cap.u.IsWindow(hwnd):return deny('target_destroyed')
    actual=cap.client(hwnd)
    if actual!=expected:return deny('client_geometry_changed',expected=list(expected),actual=list(actual))
    dpi=cap.u.GetDpiForWindow(hwnd)
    if dpi!=context['dpi']:return deny('target_dpi_changed',expected=context['dpi'],actual=dpi)
    foreground=cap.u.GetAncestor(cap.u.GetForegroundWindow(),2)
    if foreground!=hwnd:return deny('target_not_foreground',foreground=int(foreground or 0))
    if cap.u.IsIconic(hwnd):return deny('target_minimized')
    if not cap.u.IsWindowVisible(hwnd):return deny('target_hidden')
    pid = W.DWORD()
    cap.u.GetWindowThreadProcessId(hwnd, C.byref(pid))
    if pid.value != context['pid']:
        return deny('target_process_changed')
    r = context['roi']
    if (len(r) != 4 or any(type(v) is not int for v in r) or
            r[2] < 1 or r[3] < 1 or r[2]*r[3] > LIMITS.roi_pixels or
            r[0] < expected[0] or r[1] < expected[1] or
            r[0]+r[2] > expected[0]+expected[2] or r[1]+r[3] > expected[1]+expected[3]):
        raise ValueError('invalid-authorized-roi')
    found = False
    blocked = False
    allowed = set(context['surfaces'])
    affinity = cap.bind(cap.u, 'GetWindowDisplayAffinity', W.BOOL, W.HWND, C.POINTER(W.DWORD))

    @cap.CALLBACK
    def visit(other, unused):
        nonlocal found, blocked
        if other == hwnd:
            found = True
            return False
        if not cap.u.IsWindowVisible(other) or cap.u.IsIconic(other):
            return True
        cloak = W.DWORD()
        if cap.dwm.DwmGetWindowAttribute(other, 14, C.byref(cloak), 4) < 0:
            deny('occluder_cloak_query_failed',window=int(other))
            blocked = True
            return False
        if cloak.value:
            return True
        box = W.RECT()
        if not cap.u.GetWindowRect(other, C.byref(box)):
            deny('occluder_geometry_query_failed',window=int(other))
            blocked = True
            return False
        if min(r[0]+r[2], box.right) <= max(r[0], box.left) or min(r[1]+r[3], box.bottom) <= max(r[1], box.top):
            return True
        owner = W.DWORD()
        cap.u.GetWindowThreadProcessId(other, C.byref(owner))
        mode = W.DWORD()
        if other in allowed and owner.value == context['owner_pid'] and (positive or
                (affinity(other, C.byref(mode)) and mode.value == 0x11)):
            return True
        blocked = True
        deny('roi_occluded',window=int(other),pid=owner.value,rect=[box.left,box.top,box.right,box.bottom])
        return False

    cap.u.EnumWindows(visit, 0)
    if not found and not blocked:deny('target_not_enumerated')
    return found and not blocked


class Analyzer:
    def __init__(self):
        self.sample = 0
        self.baseline = None
        self.baseline_context = None
        self.watches = OrderedDict()
        self.shared = None

    def expire_shared(self):
        if self.shared is not None and not 0 <= time.perf_counter_ns()-self.shared['time'] < 100_000_000:
            self.shared = None

    def shared_pixels(self):
        return 0 if self.shared is None else self.shared['image'].size

    def retained_pixels(self):
        return sum(v[0].size for v in self.watches.values())+(0 if self.baseline is None else self.baseline.size)+self.shared_pixels()

    def share_identity(self, context):
        allowed=context.get('cache_regions',[])
        calibrations=context.get('cache_calibrations')
        if (not isinstance(calibrations,dict) or not 1<len(allowed)<=LIMITS.active_regions or
                set(calibrations)!=set(allowed) or any(not isinstance(v,str) or len(v)>128 for v in calibrations.values()) or
                calibrations.get(context.get('region_id'))!=context.get('calibration_id')):
            return None
        return (context['hwnd'],context['pid'],tuple(context['client']),context['dpi'],context['generation'],
                context['owner_pid'],tuple(context['surfaces']),tuple(sorted(allowed)),tuple(sorted(calibrations.items())))

    def remember_frame(self, context, image, sample, sampled):
        identity=self.share_identity(context)
        budget=context.get('cache_budget_pixels',LIMITS.retained_pixels)
        # Shared image is an extra, separately budgeted allocation. It never
        # evicts another region's watch cache merely to enable this optimization.
        if identity is not None and self.retained_pixels()+image.size<=budget:
            self.shared=dict(image=image,identity=identity,roi=list(context['roi']),
                             sample=sample,time=sampled,used={context['region_id']},analyses={})

    def cache_key(self, context):
        return (context.get('region_id','default'), context.get('calibration_id',''), context['generation'])

    def trim_cache(self, context, reserve=0):
        allowed=context.get('cache_regions',[context.get('region_id','default')])
        budget=context.get('cache_budget_pixels',LIMITS.retained_pixels)
        if (not isinstance(allowed,list) or len(allowed)>LIMITS.active_regions or
                any(not isinstance(v,str) or len(v)>128 for v in allowed) or
                type(budget) is not int or not 0<=budget<=LIMITS.retained_pixels):
            raise ValueError('invalid-cache-budget')
        self.expire_shared()
        if self.shared is not None and (self.shared['identity']!=self.share_identity(context) or
                sum(v[0].size for v in self.watches.values())+reserve+self.shared_pixels()>budget):
            self.shared=None
        for key in list(self.watches):
            if key[0] not in allowed or key[2]!=context['generation'] or (
                    key[0]==context.get('region_id','default') and key[1]!=context.get('calibration_id','')):
                self.watches.pop(key)
        while self.watches and sum(v[0].size for v in self.watches.values())+reserve+self.shared_pixels()>budget:
            self.watches.popitem(last=False)
        return reserve+self.shared_pixels()<=budget

    def execute(self, task):
        try:
            return self._execute(task)
        except Exception:
            self.shared=None
            raise

    def _execute(self, task):
        import cv2
        import numpy as np
        from . import capture as cap
        from .vision import learn, locate, gray
        cv2.setNumThreads(1)
        context, operation = task['context'], task['operation']
        tracking_kind=task.get('tracking_kind','boundary')
        if tracking_kind not in ('boundary','features'):
            raise ValueError('invalid-tracking-kind')
        if operation not in ('calibrate','baseline','positive','excluded','locate','watch'):
            raise ValueError('unsupported-operation')
        ordinary=operation in ('locate','watch')
        minimum_sample=task.get('minimum_sample_t_ns',0)
        if type(minimum_sample) is not int or minimum_sample<0:
            raise ValueError('invalid-minimum-sample-time')
        if not ordinary:self.shared=None
        if operation in ('calibrate','baseline'):
            self.watches.clear();self.baseline=self.baseline_context=None
        self.trim_cache(context,0 if self.baseline is None else self.baseline.size)
        # All image access remains within the main process's fixed grant.
        positive = operation == 'positive'
        if not permission(context, positive):
            self.shared=None
            return dict(status='paused', reason='capture_permission_revoked',permission_detail=dict(permission_diagnostic))
        reused=False
        if ordinary and self.shared is not None:
            self.expire_shared()
            if self.shared is not None:
                x,y,w,h=context['roi'];sx,sy,sw,sh=self.shared['roi']
                reused=(context['region_id'] not in self.shared['used'] and
                        self.shared['time']>=minimum_sample and
                        sx<=x and sy<=y and x+w<=sx+sw and y+h<=sy+sh)
        if reused:
            sampled=self.shared['time'];sample=self.shared['sample']
            image=self.shared['image'][y-sy:y-sy+h,x-sx:x-sx+w]
        else:
            self.shared=None
            sampled=time.perf_counter_ns()
            image=gray(cap.grab(context['roi']))
        if not permission(context, positive):
            self.shared=None
            return dict(status='paused', reason='capture_permission_revoked',permission_detail=dict(permission_diagnostic))
        if reused:self.shared['used'].add(context['region_id'])
        else:self.sample+=1;sample=self.sample
        result = dict(sample_id=sample, sample_t_ns=sampled, context=context,capture_reused=reused,
                      capture_roi=list(context['roi']))
        if operation == 'watch':
            cached=self.watches.get(self.cache_key(context))
            same = cached is not None and all(context[k] == cached[1][k]
                for k in ('hwnd', 'pid', 'client', 'dpi', 'roi', 'generation'))
            result.update(status='unchanged' if same and np.array_equal(gray(image), cached[0]) else 'changed')
            if not reused:self.remember_frame(context,image,sample,sampled)
            return result
        if operation in ('positive', 'excluded'):
            if self.baseline is None or any(context[k] != self.baseline_context[k]
                    for k in ('hwnd', 'pid', 'client', 'dpi', 'roi', 'generation')):
                raise ValueError('capture-proof-context-changed')
            delta = float(np.mean(np.abs(gray(image).astype(np.float32)-self.baseline)))
            result.update(status='proof', difference=delta)
            return result
        if operation in ('calibrate', 'baseline'):
            if not self.trim_cache(context,image.shape[0]*image.shape[1]):
                raise ValueError('retained-pixel-budget-exceeded')
            self.baseline = gray(image).copy()
            self.baseline_context = context.copy()
            if operation == 'baseline':
                result.update(status='baseline')
                return result
            template = learn(image, task['selection'],tracking_kind)
            import struct
            data = b'VTP1'+struct.pack('<II', template.shape[1], template.shape[0])+template.tobytes()
            result['template_hex'] = data.hex()
            result['template_sha256'] = hashlib.sha256(data).hexdigest()
        elif operation == 'locate':
            template = checked_template(task['template_hex'])
            ratio = context['dpi']/task['calibration_dpi']
            if ratio != 1:
                w, h = max(1, round(template.shape[1]*ratio)), max(1, round(template.shape[0]*ratio))
                if w*h > LIMITS.template_pixels:
                    raise ValueError('scaled-template-budget-exceeded')
                template = cv2.resize(template, (w, h), interpolation=cv2.INTER_LINEAR)
            self.baseline = self.baseline_context = None
        else:
            raise ValueError('unsupported-operation')
        analysis_key=None
        if operation=='locate':
            analysis_key=(tuple(context['roi']),hashlib.sha256(task['template_hex'].encode('ascii')).digest(),ratio,tracking_kind)
        cached_analysis=None if self.shared is None or analysis_key is None else self.shared['analyses'].get(analysis_key)
        box, reason, score = cached_analysis if cached_analysis is not None else locate(image, template,tracking_kind)
        result['analysis_reused']=cached_analysis is not None
        result.update(status='complete' if box else 'not_found', reason=reason, score=score,
                      boundary_rect=None, unique=box is not None,
                      boundary_checked=box is not None and tracking_kind=='boundary',
                      position_checked=box is not None and tracking_kind=='features')
        if box:
            result['boundary_rect'] = [box[0]+context['roi'][0], box[1]+context['roi'][1], box[2], box[3]]
            if operation=='locate':
                key=self.cache_key(context)
                for old in list(self.watches):
                    if old[0]==key[0]:self.watches.pop(old)
                if self.trim_cache(context,image.shape[0]*image.shape[1]):
                    self.watches[key]=(gray(image).copy(),context.copy())
        if ordinary and not reused:self.remember_frame(context,image,sample,sampled)
        if analysis_key is not None and self.shared is not None:
            self.shared['analyses'][analysis_key]=(box,reason,score)
        return result


def main():
    for key in SINGLE_THREAD_ENV:
        os.environ[key] = '1'
    parser = argparse.ArgumentParser()
    parser.add_argument('--pipe', required=True)
    parser.add_argument('--session', required=True)
    parser.add_argument('--parent-pid', type=int, required=True)
    parser.add_argument('--deps')
    parser.add_argument('--wake-event')
    args = parser.parse_args()
    if args.deps:
        from pathlib import Path
        sys.path.insert(0, str(Path(args.deps).resolve(strict=True)))
    from overlay.pipe_client import PipeClient, K, bind
    from .capture import dpi_init
    dpi_init()
    bind('GetNamedPipeServerProcessId', W.BOOL, W.HANDLE, C.POINTER(W.DWORD))
    bind('OpenProcess', W.HANDLE, W.DWORD, W.BOOL, W.DWORD)
    parent = K.OpenProcess(0x100000, False, args.parent_pid)
    if not parent:
        return 2
    wake=None
    try:
        with PipeClient(args.pipe, timeout_ms=2500) as pipe:
            server = W.DWORD()
            if not K.GetNamedPipeServerProcessId(pipe.handle, C.byref(server)) or server.value != args.parent_pid:
                raise ValueError('worker-server-identity-mismatch')
            if args.wake_event:
                bind('OpenEventW',W.HANDLE,W.DWORD,W.BOOL,W.LPCWSTR)
                wake=K.OpenEventW(0x100000,False,args.wake_event)
                if not wake:raise OSError('worker-wake-event-unavailable')
            analyzer, response, request_id = Analyzer(), None, 0
            while K.WaitForSingleObject(parent, 0) == 258:
                analyzer.expire_shared()
                request_id += 1
                message = dict(protocol=PROTOCOL, session=args.session, request_id=request_id,
                               op='hello' if request_id == 1 else 'next', result=response)
                if request_id==1:message['capabilities']=['multi_region_v1','contained_roi_reuse_v1','feature_position_v1']
                reply = pipe.exchange(message)
                if not reply.get('ok') or reply.get('request_id') != request_id:
                    return 2
                if reply.get('stop'):
                    return 0
                task = reply.get('task')
                response = None
                if task:
                    started = time.perf_counter()
                    try:
                        response = analyzer.execute(task)
                    except Exception as error:
                        response = dict(status='unavailable', reason=type(error).__name__+':'+str(error))
                    response.update(task_id=task['task_id'], elapsed_ms=(time.perf_counter()-started)*1000)
                    response['cached_regions']=len(analyzer.watches)
                    response['retained_pixels']=analyzer.retained_pixels()
                else:
                    delay=min(1000,max(1,reply.get('wait_ms',100)))
                    if wake:
                        handles=(W.HANDLE*2)(parent,wake)
                        result=K.WaitForMultipleObjects(2,handles,False,delay)
                        if result==0:return 0
                        if result not in (1,258):return 2
                    else:K.WaitForSingleObject(parent,delay)
    finally:
        if wake:K.CloseHandle(wake)
        K.CloseHandle(parent)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
