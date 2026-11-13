import json
import math
import os
import re
from dataclasses import asdict, dataclass, fields, field
import uuid
from pathlib import Path
from visual_tracking.schema import binding_copy


@dataclass
class Region:
    id: str = field(default_factory=lambda: uuid.uuid4().hex)
    x: float = 20
    y: float = 20
    width: float = 160
    height: float = 40
    anchor_x: str = 'left'
    anchor_y: str = 'top'
    reference_width: float = 0
    reference_height: float = 0
    position_mode: str = 'anchor'
    visual_binding: dict | None = None
    text: str = "我的工作台"
    font: str = "Segoe UI"
    size: int = 14
    bold: bool = False
    color: str = "#ececec"
    background: str = "#212121"
    opacity: int = 255
    align: str = "left"
    padding: int = 8
    letter_spacing: float = 0
    scroll: bool = False
    scroll_direction: str = "left"
    scroll_speed: int = 40
    scroll_gap: int = 40
    scroll_feather: bool = False
    scroll_feather_width: int = 24
    outline: bool = False
    outline_width: float = 1.5
    outline_color: str = "#000000"
    glow: bool = False
    glow_color: str = "#2ab89d"
    glow_radius: int = 10
    glow_strength: int = 75
    glow_pulse: bool = True
    glow_period: float = 2.0

    @classmethod
    def parse(cls, data):
        if not isinstance(data, dict):
            raise ValueError("区域配置必须是对象")
        r = cls(**{f.name: data[f.name] for f in fields(cls) if f.name in data})
        if r.position_mode not in ('anchor', 'visual'):
            raise ValueError('invalid_position_mode')
        r.visual_binding = binding_copy(r.visual_binding)
        if r.position_mode == 'visual' and r.visual_binding is None:
            raise ValueError('visual_calibration_required')
        if not isinstance(r.id, str) or not re.fullmatch(r'[0-9a-fA-F]{32}', r.id):
            raise ValueError('invalid region identifier')
        for key in ("x", "y", "width", "height"):
            value = float(getattr(r, key))
            if not math.isfinite(value):
                raise ValueError("坐标必须为有限数字")
            setattr(r, key, max(0 if key in ("x", "y") else 8, min(value, 100000)))
        r.size = max(6, min(200, int(r.size)))
        r.opacity = max(0, min(255, int(r.opacity)))
        r.padding = max(0, min(100, int(r.padding)))
        if r.anchor_x not in ('left', 'center', 'right'):
            r.anchor_x = 'left'
        if r.anchor_y not in ('top', 'center', 'bottom'):
            r.anchor_y = 'top'
        for key in ('reference_width', 'reference_height'):
            value = float(getattr(r, key))
            if not math.isfinite(value):
                raise ValueError('定位参考尺寸必须为有限数字')
            setattr(r, key, max(0, min(value, 100000)))
        if r.align not in ("left", "center", "right"):
            r.align = "left"
        for key in ("text", "font", "color", "background", "outline_color", "glow_color"):
            if not isinstance(getattr(r, key), str):
                raise ValueError(f"{key} 必须是文字")
        for key in ("color", "background", "outline_color", "glow_color"):
            if not re.fullmatch(r"#[0-9a-fA-F]{6}", getattr(r, key)):
                raise ValueError(f"{key} 必须是 #RRGGBB 颜色")
        r.bold = bool(r.bold)
        for key in ("scroll", "scroll_feather", "outline", "glow", "glow_pulse"):
            setattr(r, key, bool(getattr(r, key)))
        if r.scroll_direction not in ("left", "right"):
            r.scroll_direction = "left"
        for key, low, high in (("letter_spacing", -10, 50), ("scroll_feather_width", 1, 200), ("scroll_speed", 5, 300), ("scroll_gap", 8, 300), ("outline_width", 0.5, 12), ("glow_radius", 1, 24), ("glow_strength", 0, 100), ("glow_period", 0.5, 10)):
            value = float(getattr(r, key))
            if not math.isfinite(value):
                raise ValueError(f"{key} 必须为有限数字")
            value = max(low, min(high, value))
            setattr(r, key, value if key in ("letter_spacing", "outline_width", "glow_period") else int(value))
        return r


class Store:
    def __init__(self, path=None):
        self.path = Path(path) if path else Path(os.environ.get("APPDATA", str(Path.home()))) / "ChatGPTOverlay" / "config.json"

    def load(self, allow_visual=False):
        if not self.path.exists():
            return []
        data = json.loads(self.path.read_text(encoding="utf-8"))
        if not isinstance(data, dict) or type(data.get('version')) is not int or data['version'] not in (1, 2) or not isinstance(data.get("regions"), list):
            raise ValueError("配置版本或格式不支持")
        regions = [Region.parse(r) for r in data["regions"]]
        if data['version'] == 1 and any(r.position_mode != 'anchor' or r.visual_binding is not None for r in regions):
            raise ValueError('visual_requires_config_v2')
        if data['version'] == 2 and not allow_visual:
            raise ValueError('视觉配置请使用原生后端，旧后端不能安全编辑')
        return regions

    def save(self, regions):
        if any(r.position_mode != 'anchor' or r.visual_binding is not None for r in regions):
            raise ValueError('视觉校准只能由原生主进程保存')
        if self.path.exists():
            current = json.loads(self.path.read_text(encoding='utf-8'))
            if not isinstance(current, dict) or type(current.get('version')) is not int or current['version'] != 1:
                raise ValueError('禁止旧后端覆盖新版配置')
        self.path.parent.mkdir(parents=True, exist_ok=True)
        temp = self.path.with_suffix(".tmp")
        temp.write_text(json.dumps({"version": 1, "regions": [asdict(r) for r in regions]}, ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(temp, self.path)
