"""Compact desktop editor styling, shared across its native Qt controls."""
from pathlib import Path

_STYLE = '''
QWidget { color: #f2f2f2; font-family: "Microsoft YaHei UI", "Segoe UI"; font-size: 13px; background: transparent; }
QDialog#inspector { background: #202226; border: 1px solid #454545; }
QWidget#navigation { background: #202226; }
QWidget#propertyPage { background: transparent; }
QStackedWidget { background: transparent; border: none; }
QLabel#appTitle { font-size: 15px; font-weight: 600; }
QLabel#sectionTitle { font-size: 18px; font-weight: 600; }
QLabel#hint, QLabel#sectionDescription { color: #b8b8b8; font-size: 12px; }
QLabel#connectionStatus { color: #b5d9f8; font-size: 12px; }
QLabel#feedback { background: transparent; color: #b8c2ce; padding: 0 8px; font-size: 12px; }
QToolButton { background: transparent; border: 1px solid transparent; border-radius: 4px; padding: 5px 7px; color: #d5d5d5; }
QToolButton:hover { background: #343434; }
QToolButton:pressed { background: #3d3d3d; }
QToolButton:checked { background: #363b40; color: #ffffff; border-color: #4d6070; }
QToolButton:focus, QPushButton:focus { border: 1px solid #9acbfa; }
QToolButton#iconControl { padding: 0; }
QToolButton#iconControl::menu-indicator { image: none; width: 0; }
QToolButton#category { text-align: left; }
QToolButton#category:checked { border: 1px solid transparent; border-bottom: 2px solid #9acbfa; background: transparent; }
QToolButton#disclosure { text-align: left; font-size: 12px; color: #c5c5c5; padding: 6px 0; }
QToolButton#disclosure:checked { background: transparent; border-color: transparent; }
QToolButton#segment { background: #24272c; border: none; min-height: 24px; min-width: 28px; }
QToolButton#segment:checked { background: #384454; color: #e7e9ee; }
QToolButton#stepper { background: #333333; border: 1px solid #454545; padding: 0; }
QToolButton#stepper:hover { background: #414141; }
QToolButton#panelClose, QToolButton#panelClose:hover, QToolButton#panelClose:pressed { background: transparent; padding: 0; }
QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background: #292c31; border: 1px solid #484848; border-bottom-color: #484848; border-radius: 4px;
    padding: 5px 8px; selection-background-color: #385d7a;
}
QSpinBox, QDoubleSpinBox { min-height: 20px; }
QLineEdit:focus, QTextEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus { border-bottom: 2px solid #9acbfa; }
QComboBox { min-height: 20px; padding-right: 26px; }
QComboBox::drop-down { border: none; width: 24px; }
QComboBox::down-arrow { image: url("@CHEVRON@"); width: 12px; height: 12px; }
QComboBox QAbstractItemView { background: #292c31; color: #f2f2f2; selection-background-color: #385d7a; border: 1px solid #555555; padding: 3px; }
QListWidget { background: transparent; border: none; outline: none; padding: 0; }
QListWidget::item { border: 1px solid transparent; border-radius: 5px; padding: 6px 32px 6px 8px; }
QListWidget::item:hover { background: #2d2d2d; }
QListWidget::item:selected { color: #f2f2f2; background: #333e48; border-color: #526a7e; }
QPushButton { background: #363636; border: 1px solid #4c4c4c; border-radius: 4px; padding: 6px 10px; min-height: 18px; }
QPushButton:hover { background: #414141; }
QPushButton:pressed { background: #2c2c2c; }
QPushButton#finish, QPushButton#primary { background: #9acbfa; color: #172c3e; border-color: #9acbfa; font-weight: 600; }
QPushButton#finish:hover, QPushButton#primary:hover { background: #b6dcff; }
QPushButton#delete { color: #edb8b6; background: transparent; border-color: transparent; }
QPushButton#delete:hover { background: #3f3030; }
QPushButton#colorButton { text-align: left; }
QScrollArea { border: none; background: transparent; }
QScrollBar:vertical { background: transparent; width: 7px; margin: 0; }
QScrollBar::handle:vertical { background: #666666; min-height: 30px; border-radius: 3px; }
QScrollBar::handle:vertical:hover { background: #999999; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QSlider::groove:horizontal { height: 3px; background: #626262; border-radius: 1px; }
QSlider::sub-page:horizontal { background: #9acbfa; }
QSlider::handle:horizontal { width: 14px; margin: -6px 0; background: #c9e4fc; border: 1px solid #9acbfa; border-radius: 7px; }
QSlider:focus { border: 1px solid #9acbfa; border-radius: 4px; }
QSplitter::handle { background: transparent; width: 8px; }
QSplitter::handle:hover { background: #3d3d3d; }
QWidget:disabled { color: #868686; }
'''.replace('@CHEVRON@',(Path(__file__).parent/'assets'/'chevron-down.png').as_posix())


DARK = dict(window='#202226', text='#f2f2f2', muted='#b8b8b8', accent='#9acbfa', error='#efb3af', success='#b5f3df')
LIGHT = dict(window='#f5f6f8', text='#20242b', muted='#596371', accent='#175eac', error='#b3261e', success='#176342')
# Map the existing semantic roles as a whole; never invert an image or font.
_LIGHT_COLORS = {
 '#f2f2f2':'#20242b', '#202226':'#f5f6f8', '#454545':'#c5cad2',
 '#1c1e22':'#f5f6f8', '#b8b8b8':'#596371', '#b5d9f8':'#285e92',
 '#b8c2ce':'#596371', '#d5d5d5':'#343b46', '#343434':'#e5e8ed',
 '#3d3d3d':'#dce2ea', '#363b40':'#e1eaf5', '#ffffff':'#182b42',
 '#4d6070':'#9fb6cf', '#9acbfa':'#175eac', '#c5c5c5':'#596371',
 '#24272c':'#eceff3', '#384454':'#dce9f9', '#e7e9ee':'#203d60',
 '#333333':'#eef1f5', '#414141':'#e2e7ee', '#292c31':'#ffffff',
 '#484848':'#c7ced8', '#385d7a':'#c6ddfa', '#555555':'#b3bdc9',
 '#2d2d2d':'#e9edf3', '#333e48':'#e0ebf8', '#526a7e':'#9bb8d8',
 '#363636':'#ffffff', '#4c4c4c':'#c7ced8', '#2c2c2c':'#dbe2eb',
 '#172c3e':'#ffffff', '#b6dcff':'#236cbb', '#edb8b6':'#b3261e',
 '#3f3030':'#fbe7e7', '#666666':'#adb6c3', '#999999':'#79889a',
 '#626262':'#bbc4cf', '#c9e4fc':'#175eac', '#868686':'#78818e',
}


def theme_style(mode):
    import re
    style=_STYLE
    if mode=='light':
        style=re.sub(r'#[0-9a-fA-F]{6}',lambda m:_LIGHT_COLORS.get(m[0],m[0]),style)
        style=style.replace('chevron-down.png','chevron-down-light.png')
    return style + '\nQMenu { background: '+('#ffffff' if mode=='light' else '#292c31')+'; border: 1px solid '+('#c7ced8' if mode=='light' else '#484848')+'; padding: 4px; } QMenu::item { padding: 6px 14px; } QMenu::item:selected { background: '+('#dce9f9' if mode=='light' else '#384454')+'; }'


def editor_font():
    from PySide6.QtGui import QFont
    font=QFont('Microsoft YaHei UI',9)
    return font


def toolbar_icon(kind, color):
    from math import cos, pi, sin
    from PySide6.QtCore import QPointF, QRectF, Qt
    from PySide6.QtGui import QColor, QIcon, QPainter, QPainterPath, QPen, QPixmap, QPolygonF

    pixmap = QPixmap(48, 48)
    pixmap.setDevicePixelRatio(2)
    pixmap.fill(Qt.GlobalColor.transparent)
    painter = QPainter(pixmap)
    painter.setRenderHint(QPainter.RenderHint.Antialiasing)
    painter.setPen(QPen(QColor(color), 1.7, Qt.PenStyle.SolidLine,
                        Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))

    if kind == 'regions':
        painter.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2)
        painter.drawLine(QPointF(9, 5), QPointF(9, 19))
    elif kind == 'settings':
        outline = QPolygonF()
        for tooth in range(8):
            for offset, radius in ((-0.5, 7), (-0.26, 9.2), (0.26, 9.2), (0.5, 7)):
                angle = (tooth + offset) * pi / 4
                outline.append(QPointF(12 + cos(angle) * radius, 12 + sin(angle) * radius))
        painter.drawPolygon(outline)
        painter.drawEllipse(QPointF(12, 12), 3.2, 3.2)
    elif kind == 'dark':
        moon = QPainterPath(QPointF(20.8, 14.1))
        moon.cubicTo(18.6, 21.6, 8.3, 22.7, 4.3, 16.3)
        moon.cubicTo(0.6, 10.5, 4.6, 3.4, 11.3, 3.1)
        moon.cubicTo(7.4, 9.3, 13.9, 17.3, 20.8, 14.1)
        moon.closeSubpath()
        painter.drawPath(moon)
    elif kind == 'light':
        painter.drawEllipse(QPointF(12, 12), 4, 4)
        for ray in range(8):
            angle = ray * pi / 4
            x, y = cos(angle), sin(angle)
            painter.drawLine(QPointF(12 + 7*x, 12 + 7*y), QPointF(12 + 9*x, 12 + 9*y))
    else:
        painter.drawRoundedRect(QRectF(3, 4, 18, 13), 2, 2)
        painter.drawLine(QPointF(12, 17), QPointF(12, 21))
        painter.drawLine(QPointF(8, 21), QPointF(16, 21))

    painter.end()
    return QIcon(pixmap)


EDITOR_STYLE=theme_style('dark')


def apply_text_rendering(root):
    """Set render hints after QSS resolves each control's font and size."""
    from PySide6.QtGui import QFont
    from PySide6.QtWidgets import QWidget,QComboBox
    root.ensurePolished()
    for combo in root.findChildren(QComboBox):combo.view()  # Include popup text.
    for widget in [root,*root.findChildren(QWidget)]:
        font=QWidget.font(widget)
        font.setHintingPreference(QFont.HintingPreference.PreferNoHinting)
        font.setStyleStrategy(QFont.StyleStrategy.PreferAntialias | QFont.StyleStrategy.NoSubpixelAntialias)
        widget.setFont(font)
