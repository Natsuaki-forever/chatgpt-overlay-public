"""Development entry. Dependencies are explicit; no experiment paths inferred."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
try:
    from visual_tracking.live_worker import main
    code=main()
except Exception:
    if not getattr(sys,'frozen',False):
        raise
    # This is an internal, windowless child. A disconnected pipe must report
    # failure by exit status, never by a freezer traceback dialog stealing focus.
    code=2
raise SystemExit(code)
