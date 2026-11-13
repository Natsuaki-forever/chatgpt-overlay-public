"""Area-aware region grouping and normal-mode layered window management."""
from dataclasses import dataclass
from PySide6.QtCore import QRect, QRectF, QPointF, QTimer, Qt
from . import native, effects


@dataclass
class Group:
    indices: tuple
    bounds: QRect


def grouped_regions(regions, viewport, dpr, max_groups=8, merge_pixels=32768):
    groups = []
    for index, region in enumerate(regions):
        bounds = QRectF(region.x, region.y, region.width, region.height).toAlignedRect().intersected(viewport)
        if not bounds.isEmpty():
            groups.append(Group((index,), bounds))
    area = lambda r: r.width()*r.height()
    while len(groups) > 1:
        candidates = []
        for i, a in enumerate(groups):
            for j in range(i+1, len(groups)):
                b = groups[j]
                union = a.bounds.united(b.bounds)
                overhead = (area(union)-area(a.bounds)-area(b.bounds))*dpr*dpr
                if a.bounds.intersects(b.bounds) or overhead <= merge_pixels or len(groups) > max_groups:
                    candidates.append((overhead, i, j, union))
        if not candidates:
            break
        _, i, j, bounds = min(candidates, key=lambda value: value[:3])
        merged = Group(tuple(sorted(groups[i].indices+groups[j].indices)), bounds)
        groups = [g for k, g in enumerate(groups) if k not in (i, j)]+[merged]
    return sorted(groups, key=lambda g: min(g.indices))


class SurfaceManager:
    def __init__(self, controller, canvas_type):
        self.controller, self.canvas_type = controller, canvas_type
        self.windows = []
        self.signature = None
        self.timer = QTimer(controller)
        self.timer.setTimerType(Qt.TimerType.PreciseTimer)
        self.timer.setInterval(33)
        self.timer.timeout.connect(self.animate)
        self.physical_pixels = 0

    def animate(self):
        for window in self.windows:
            if window.isVisible():
                for region in window.regions:
                    if effects.animated(region):
                        window.update(window.rect_for(region).toAlignedRect())

    def hide(self):
        self.timer.stop()
        for window in self.windows:
            window.hide()
        self.physical_pixels = 0

    def close(self):
        self.hide()
        for window in self.windows:
            native.configure(int(window.winId()), None, False)
            window.deleteLater()
        self.windows.clear()
        self.signature = None

    def sync(self, target, box, screen, geometry):
        c = self.controller
        groups = grouped_regions(c.regions, QRect(0, 0, geometry.width(), geometry.height()), screen.devicePixelRatio())
        signature = tuple((tuple(c.regions[i].id for i in group.indices), group.bounds.getRect()) for group in groups)
        if signature != self.signature:
            self.close()
            self.signature = signature
            for group in groups:
                window = self.canvas_type([c.regions[i] for i in group.indices])
                window.animate_enabled = False
                window.clock = c.canvas.clock
                self.windows.append(window)
        sx, sy = box[2]/geometry.width(), box[3]/geometry.height()
        own_handles = {int(window.winId()) for window in self.windows}
        anchor = native.u.GetWindow(target, 3)
        while anchor in own_handles:
            anchor = native.u.GetWindow(anchor, 3)
        if anchor and native.u.GetWindowLongPtrW(anchor, -20) & 0x8:
            anchor = None
        self.physical_pixels = 0
        for window, group in zip(self.windows, groups):
            # Refresh references if a configuration update preserved the IDs.
            window.regions = [c.regions[i] for i in group.indices]
            window.region_origin = QPointF(group.bounds.topLeft())
            handle = int(window.winId())
            logical = QRect(geometry.topLeft()+group.bounds.topLeft(), group.bounds.size())
            physical = (box[0]+round(group.bounds.x()*sx), box[1]+round(group.bounds.y()*sy),
                        max(1, round(group.bounds.width()*sx)), max(1, round(group.bounds.height()*sy)))
            if window.windowHandle().screen() is not screen:
                window.windowHandle().setScreen(screen)
            if native.client_box(handle) != physical and window.geometry() != logical:
                window.setGeometry(logical)
            native.configure(handle, target, False)
            window.show()
            native.configure(handle, target, False)
            same_order = native.u.GetWindow(handle, 3) == anchor if anchor else not native.u.GetWindow(handle, 3)
            if native.client_box(handle) != physical or not same_order:
                flags = 0x0010 | 0x0200 | (0x0004 if same_order else 0)
                native.u.SetWindowPos(handle, anchor or 0, *physical, flags)
            anchor = handle
            self.physical_pixels += physical[2]*physical[3]
        if any(effects.animated(r) for r in c.regions) and self.windows:
            self.timer.start() if not self.timer.isActive() else None
        else:
            self.timer.stop()

    def visible(self):
        return any(window.isVisible() for window in self.windows)
