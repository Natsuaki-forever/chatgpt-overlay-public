"""Stage-1 snapshot probe over inherited stdio, NOT the product pipe worker."""
import argparse
from dataclasses import asdict
import json
import os
from pathlib import Path
import struct
import sys
import time

from .contracts import LIMITS, PROBE_PROTOCOL, SINGLE_THREAD_ENV
from .resources import memory


def load_png(root, name):
    if not isinstance(name, str):
        raise ValueError('invalid-image-path')
    path = (root/name).resolve()
    if not path.is_relative_to(root):
        raise ValueError('image-outside-probe-directory')
    with path.open('rb') as stream:
        header = stream.read(24)
        if (len(header) != 24 or header[:8] != b'\x89PNG\r\n\x1a\n' or
                header[12:16] != b'IHDR'):
            raise ValueError('expected-png')
        w, h = struct.unpack('>II', header[16:24])
        if not w or not h or w*h > LIMITS.roi_pixels:
            raise ValueError('roi-pixel-budget-exceeded')
        stream.seek(0)
        data = stream.read(LIMITS.png_bytes+1)
    if len(data) > LIMITS.png_bytes:
        raise ValueError('png-byte-budget-exceeded')
    import cv2
    import numpy as np
    image = cv2.imdecode(np.frombuffer(data, np.uint8), cv2.IMREAD_COLOR)
    if image is None or image.shape[:2] != (h, w):
        raise ValueError('invalid-png')
    return image


def dispatch(request, root):
    if not isinstance(request, dict) or request.get('protocol') != PROBE_PROTOCOL:
        raise ValueError('protocol-mismatch')
    if set(request)-{'protocol', 'request_id', 'op', 'payload'}:
        raise ValueError('unexpected-field')
    if type(request.get('request_id')) is not int or request['request_id'] < 0:
        raise ValueError('invalid-request-id')
    op = request.get('op')
    if op == 'hello':
        return dict(role='stage1-snapshot-probe', capabilities=['snapshot_assessment'],
                    product_integration=False, limits=asdict(LIMITS))
    if op != 'assess':
        raise ValueError('unsupported-operation')
    payload = request.get('payload')
    if not isinstance(payload, dict) or set(payload) != {'image', 'selection_rect', 'dpi'}:
        raise ValueError('invalid-assessment-payload')
    image = load_png(root, payload['image'])
    import cv2
    cv2.setNumThreads(1)
    from .vision import assess
    return assess(image, payload['selection_rect'], payload['dpi'])


def main():
    # NumPy's BLAS runtime starts threads at import, before cv2.setNumThreads.
    # This isolated worker has one task; reserve no per-core BLAS work pools.
    for name in SINGLE_THREAD_ENV:
        os.environ[name] = '1'
    parser = argparse.ArgumentParser()
    parser.add_argument('--probe-dir', type=Path, required=True)
    parser.add_argument('--deps', type=Path)
    args = parser.parse_args()
    if args.deps:
        sys.path.insert(0, str(args.deps.resolve(strict=True)))
    root = args.probe_dir.resolve(strict=True)
    if not root.is_dir():
        parser.error('probe-dir must be a directory')
    for raw in iter(lambda: sys.stdin.buffer.readline(LIMITS.message_bytes+1), b''):
        if len(raw) > LIMITS.message_bytes:
            print(json.dumps(dict(ok=False, error='message-too-large')), flush=True)
            return 2  # Do not interpret the tail as another request.
        started = time.perf_counter()
        request = None
        try:
            def invalid_constant(value):
                raise ValueError('non-finite-json')
            request = json.loads(raw.decode('utf-8'), parse_constant=invalid_constant)
            result = dispatch(request, root)
            response = dict(ok=True, result=result)
        except (ValueError, OSError, ImportError) as error:
            response = dict(ok=False, error=str(error))
        except Exception as error:
            response = dict(ok=False, error='analysis-error:'+type(error).__name__)
        response.update(protocol=PROBE_PROTOCOL,
                        request_id=request.get('request_id') if isinstance(request, dict) else None,
                        elapsed_ms=round((time.perf_counter()-started)*1000, 3),
                        memory=memory(), qt_loaded=any(n.startswith('PySide6') for n in sys.modules))
        print(json.dumps(response, ensure_ascii=True, allow_nan=False), flush=True)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
