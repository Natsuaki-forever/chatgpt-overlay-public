"""Stage-0 budgets; no product configuration migration is performed here."""
from dataclasses import dataclass
import math


PROBE_PROTOCOL = 'visual-probe-1'
CONFIG_VERSION_WITH_VISUAL = 2
SINGLE_THREAD_ENV = ('OPENBLAS_NUM_THREADS', 'OMP_NUM_THREADS', 'MKL_NUM_THREADS', 'NUMEXPR_NUM_THREADS')


@dataclass(frozen=True)
class Limits:
    roi_pixels: int = 524288
    template_pixels: int = 131072
    message_bytes: int = 1048576
    png_bytes: int = 4194304
    active_regions: int = 4
    retained_pixels: int = 2097152
    compute_ms_per_second: int = 100
    inflight_tasks: int = 1
    latest_pending_per_region: int = 1
    idle_exit_seconds: float = 5.0
    edge_dip: float = 32.0
    size_ratio: float = 1.25
    settle_seconds: float = .35
    watch_seconds: float = .25
    audit_seconds: float = 1.0
    result_age_seconds: float = 1.0
    audit_expiry_seconds: float = 2.0
    confirmation_seconds: float = .08
    failures_before_lock: int = 3


LIMITS = Limits()


def integer_rect(value, width, height):
    if (not isinstance(value, (list, tuple)) or len(value) != 4 or
            any(type(v) is not int for v in value)):
        raise ValueError('invalid-selection-rect')
    x, y, w, h = value
    if x < 0 or y < 0 or w < 12 or h < 12 or x+w > width or y+h > height:
        raise ValueError('selection-outside-image')
    return x, y, w, h


def checked_dpi(value):
    if type(value) not in (int, float) or not math.isfinite(value) or not 48 <= value <= 768:
        raise ValueError('invalid-dpi')
    return float(value)
