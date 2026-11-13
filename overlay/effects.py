"""Cached text layers: shaping/fallback once, translation/opacity per frame."""
import math
from functools import lru_cache, wraps
from collections import OrderedDict, namedtuple

from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QFont, QImage, QLinearGradient, QPainter, QPainterPath, QPen, QTextLayout, QTextCharFormat, QRawFont


def text_layout(text, font, width, height, align, scrolling):
    """Use an explicit Chinese fallback without scanning every installed font.

    Preserve the selected font for every character it supports. Qt's automatic
    Windows font merging can otherwise populate a very large fallback database.
    """
    text = ' '.join(text.splitlines()) if scrolling else text.replace('\n', '\u2028')
    layout = QTextLayout(text, font)
    primary = QRawFont.fromFont(font)
    fallback = None
    ranges = []
    utf16_offset = 0
    for char in text:
        count = 2 if ord(char) > 0xffff else 1
        if not char.isspace() and not primary.supportsCharacter(ord(char)):
            if fallback is None:
                chinese = QFont(font)
                chinese.setFamily('Microsoft YaHei UI')
                fallback = QRawFont.fromFont(chinese)
            if fallback.supportsCharacter(ord(char)):
                if ranges and ranges[-1].start+ranges[-1].length == utf16_offset:
                    ranges[-1].length += count
                else:
                    span = QTextLayout.FormatRange()
                    span.start, span.length = utf16_offset, count
                    span.format = QTextCharFormat()
                    span.format.setFontFamilies(['Microsoft YaHei UI'])
                    ranges.append(span)
        utf16_offset += count
    layout.setFormats(ranges)
    option = layout.textOption()
    option.setWrapMode(option.WrapMode.NoWrap if scrolling else option.WrapMode.WrapAtWordBoundaryOrAnywhere)
    layout.setTextOption(option)
    y = 0
    layout.beginLayout()
    while True:
        line = layout.createLine()
        if not line.isValid():
            break
        line.setLineWidth(1000000 if scrolling else max(1, width))
        x = 0 if scrolling or align == 'left' else (width-line.naturalTextWidth()) / (2 if align == 'center' else 1)
        line.setPosition(QPointF(x, y))
        y += line.height()
    layout.endLayout()
    return layout, y


def byte_cache(budget):
    """Bound retained image bytes rather than only the number of text styles."""
    def decorate(function):
        entries = OrderedDict()
        total = hits = misses = 0
        info = namedtuple('CacheInfo', 'hits misses bytes entries')
        @wraps(function)
        def cached(*args):
            nonlocal total, hits, misses
            if args in entries:
                hits += 1
                result, size = entries.pop(args)
                entries[args] = result, size
                return result
            misses += 1
            result = function(*args)
            images = result if isinstance(result, tuple) else (result,)
            size = sum(value.sizeInBytes() for value in images if isinstance(value, QImage))
            if size <= budget:
                while entries and (total+size > budget or len(entries) >= 16):
                    _, (_, old_size) = entries.popitem(last=False)
                    total -= old_size
                entries[args] = result, size
                total += size
            return result
        def clear():
            nonlocal total, hits, misses
            entries.clear()
            total = hits = misses = 0
        cached.cache_clear = clear
        cached.cache_info = lambda: info(hits, misses, total, len(entries))
        return cached
    return decorate


def animated(region):
    return bool(region.text and (region.scroll or (region.glow and region.glow_pulse and region.glow_strength)))


@byte_cache(8*1024*1024)
def layers(text, family, size, bold, width, height, align, scrolling,
           color, outline, outline_width, outline_color,
           glow, glow_radius, glow_color, dpr, letter_spacing=0):
    font = QFont(family)
    font.setPixelSize(size)
    font.setBold(bold)
    font.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, letter_spacing)
    layout, y = text_layout(text, font, width, height, align, scrolling)
    path = QPainterPath()
    for run in layout.glyphRuns():
        raw = run.rawFont()
        for glyph, position in zip(run.glyphIndexes(), run.positions()):
            shape = raw.pathForGlyph(glyph)
            shape.translate(position.x(), position.y() + (height-y)/2)
            path.addPath(shape)
    advance = max((layout.lineAt(i).naturalTextWidth() for i in range(layout.lineCount())), default=0)
    pad = (glow_radius if glow else 0) + (outline_width if outline else 0) + 2
    bounds = path.boundingRect().adjusted(-pad, -pad, pad, pad)
    if bounds.isEmpty():
        return None
    # Bound cached bitmap allocation for oversized user rectangles / text.
    if bounds.width()*dpr > 16384 or bounds.height()*dpr > 8192 or bounds.width()*bounds.height()*dpr*dpr > 1000000:
        return None
    def image():
        result = QImage(max(1, math.ceil(bounds.width()*dpr)), max(1, math.ceil(bounds.height()*dpr)), QImage.Format.Format_ARGB32_Premultiplied)
        result.setDevicePixelRatio(dpr)
        result.fill(Qt.GlobalColor.transparent)
        return result
    core, halo = image(), image() if glow else None
    p = QPainter(core)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    p.translate(-bounds.x(), -bounds.y())
    if outline:
        p.strokePath(path, QPen(QColor(outline_color), outline_width*2, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
    # Fill after the stroke so outlines never consume thin letter interiors.
    p.fillPath(path, QColor(color))
    p.end()
    if halo is not None:
        p = QPainter(halo)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.translate(-bounds.x(), -bounds.y())
        p.setBrush(Qt.BrushStyle.NoBrush)
        for step in range(8, 0, -1):
            tint = QColor(glow_color)
            tint.setAlphaF(0.035 + (1-step/8)*0.1)
            p.setPen(QPen(tint, 2*(glow_radius*step/8 + (outline_width if outline else 0)), Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
            p.drawPath(path)
        p.end()
    return core, halo, bounds.topLeft(), advance


def draw(painter, region, rect, seconds, dpr, _feathered=False):
    if region.scroll and region.scroll_feather and not _feathered:
        # Mask only an isolated text layer: never erase the rectangle background
        # or other regions. Match the device DPR to avoid blurred HiDPI text.
        w, h = math.ceil(rect.width()*dpr), math.ceil(rect.height()*dpr)
        if 0 < w*h <= 1000000:
            frame = scratch_frame(w, h, dpr)
            frame.fill(Qt.GlobalColor.transparent)
            p = QPainter(frame)
            local = QRectF(0, 0, rect.width(), rect.height())
            p.setClipRect(local)
            success = draw(p, region, local, seconds, dpr, _feathered=True)
            p.setCompositionMode(QPainter.CompositionMode.CompositionMode_DestinationIn)
            p.drawImage(QPointF(0, 0), feather_mask(w, h, dpr, region.scroll_feather_width))
            p.end()
            if success:
                painter.drawImage(rect.topLeft(), frame)
            return success
    width = max(1, rect.width()-2*region.padding)
    cached = layers(region.text, region.font, region.size, region.bold, width, rect.height(), region.align,
                    region.scroll, region.color, region.outline, region.outline_width, region.outline_color,
                    region.glow, region.glow_radius, region.glow_color, dpr, region.letter_spacing)
    if cached is None:
        # Oversized text keeps the original lightweight Qt renderer.
        return False
    core, halo, origin, advance = cached
    base = rect.topLeft() + QPointF(region.padding, 0)
    positions = [0]
    if region.scroll:
        period = max(advance, width) + region.scroll_gap
        phase = (seconds*region.scroll_speed) % period
        offset = -phase if region.scroll_direction == 'left' else phase-period
        positions = [offset, offset+period]
    for offset in positions:
        point = base + origin + QPointF(offset, 0)
        if halo is not None:
            opacity = region.glow_strength/100
            if region.glow_pulse:
                opacity *= 0.25 + 0.75*(0.5+0.5*math.cos(seconds*math.tau/region.glow_period))
            painter.setOpacity(opacity)
            painter.drawImage(point, halo)
            painter.setOpacity(1)
        painter.drawImage(point, core)
    return True


@lru_cache(maxsize=1)
def scratch_frame(width, height, dpr):
    frame = QImage(width, height, QImage.Format.Format_ARGB32_Premultiplied)
    frame.setDevicePixelRatio(dpr)
    return frame


@byte_cache(4*1024*1024)
def feather_mask(width, height, dpr, feather):
    mask = QImage(width, height, QImage.Format.Format_ARGB32_Premultiplied)
    mask.setDevicePixelRatio(dpr)
    logical_width = width/dpr
    fraction = min(0.5, feather/logical_width)
    gradient = QLinearGradient(0, 0, logical_width, 0)
    gradient.setColorAt(0, QColor(255, 255, 255, 0))
    gradient.setColorAt(fraction, QColor(255, 255, 255, 255))
    if fraction < 0.5:
        gradient.setColorAt(1-fraction, QColor(255, 255, 255, 255))
    gradient.setColorAt(1, QColor(255, 255, 255, 0))
    mask.fill(Qt.GlobalColor.transparent)
    p = QPainter(mask)
    p.fillRect(QRectF(0, 0, logical_width, height/dpr), gradient)
    p.end()
    return mask


def release_buffers():
    layers.cache_clear()
    feather_mask.cache_clear()
    scratch_frame.cache_clear()
