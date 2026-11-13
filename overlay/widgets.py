"""Small native Qt controls; no browser runtime or third-party UI framework."""
from PySide6.QtCore import Qt, QSize, QObject, QEvent, Signal
from PySide6.QtGui import QColor, QPainter, QPen, QFont, QFontDatabase, QPalette
from PySide6.QtWidgets import (
    QAbstractSpinBox, QCheckBox, QDoubleSpinBox, QSpinBox, QSlider,
    QHBoxLayout, QVBoxLayout, QToolButton, QWidget, QApplication, QComboBox,
)


class Disclosure(QWidget):
    def __init__(self,title):
        super().__init__()
        layout=QVBoxLayout(self);layout.setContentsMargins(0,0,0,0);layout.setSpacing(4)
        self.toggle=QToolButton();self.toggle.setObjectName('disclosure');self.toggle.setText(title)
        self.toggle.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextBesideIcon)
        self.toggle.setCheckable(True);self.toggle.setArrowType(Qt.ArrowType.RightArrow)
        self.body=QWidget();body=QVBoxLayout(self.body);body.setContentsMargins(8,4,0,4);body.setSpacing(8)
        layout.addWidget(self.toggle);layout.addWidget(self.body);self.body.hide()
        self.toggle.toggled.connect(self.set_expanded)

    def set_expanded(self,expanded):
        self.toggle.setChecked(expanded);self.body.setVisible(expanded)
        self.toggle.setArrowType(Qt.ArrowType.DownArrow if expanded else Qt.ArrowType.RightArrow)


class FontPicker(QComboBox):
    """Family names in the UI font; never render a list with every installed font."""
    currentFontChanged = Signal(QFont)

    def __init__(self, *, defer_families=False):
        super().__init__()
        self.setEditable(True)
        self.setInsertPolicy(QComboBox.InsertPolicy.NoInsert)
        self.setMaxVisibleItems(12)
        self.lineEdit().setMaxLength(128)
        self.completer().setCaseSensitivity(Qt.CaseSensitivity.CaseInsensitive)
        self.completer().setFilterMode(Qt.MatchFlag.MatchContains)
        self._committed=''
        self.activated.connect(self.commit)
        self.lineEdit().editingFinished.connect(self.commit)
        self.setToolTip('按字体名称搜索；列表统一显示，确认选择后应用字体。')
        if not defer_families:
            self.load_families()

    def load_families(self):
        self.addItems(QFontDatabase.families())
        self._committed=self.currentText()

    def currentFont(self):
        return QFont(self._committed)

    def setCurrentFont(self,font):
        name=font.family()
        if self.findText(name)<0:self.addItem(name)
        self.setCurrentText(name);self._committed=name

    def commit(self,*args):
        name=self.currentText().strip()
        if not name:self.setCurrentText(self._committed);return
        if name!=self._committed:
            self._committed=name
            self.currentFontChanged.emit(QFont(name))


class SpinBox(QSpinBox):
    def __init__(self,*args,**kwargs):
        super().__init__(*args,**kwargs)
        self.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
        self.setKeyboardTracking(False)
    def wheelEvent(self, event):
        event.ignore()  # Scrolling a page must never silently change a setting.


class DoubleSpinBox(QDoubleSpinBox):
    def __init__(self,*args,**kwargs):
        super().__init__(*args,**kwargs)
        self.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
        self.setKeyboardTracking(False)
    def wheelEvent(self, event):
        event.ignore()


class Slider(QSlider):
    def __init__(self):
        super().__init__(Qt.Orientation.Horizontal)
        self.setMinimumWidth(60)
        self.setMinimumHeight(32)

    def wheelEvent(self, event):
        event.ignore()

    def mousePressEvent(self, event):
        # A track click goes directly to the clicked value instead of stepping
        # one page at a time. The native handle keeps normal drag/key behavior.
        if event.button() == Qt.MouseButton.LeftButton:
            from PySide6.QtWidgets import QStyle, QStyleOptionSlider
            option = QStyleOptionSlider()
            self.initStyleOption(option)
            handle = self.style().subControlRect(QStyle.ComplexControl.CC_Slider, option, QStyle.SubControl.SC_SliderHandle, self)
            if not handle.contains(event.position().toPoint()):
                span = max(1, self.width()-handle.width())
                position = round(event.position().x()-handle.width()/2)
                self.setValue(QStyle.sliderValueFromPosition(self.minimum(), self.maximum(), position, span, option.upsideDown))
        super().mousePressEvent(event)


class _EnabledState(QObject):
    def __init__(self,spin,row):
        super().__init__(row);self.spin=spin;self.row=row;spin.installEventFilter(self)
    def eventFilter(self,watched,event):
        if event.type()==QEvent.Type.EnabledChange:
            for child in self.row.findChildren(QWidget):
                if child is not self.spin and not self.spin.isAncestorOf(child):child.setEnabled(self.spin.isEnabled())
        return False


def sync_number(spin,value,force=False):
    focus=QApplication.focusWidget()
    if not force and focus and (focus is spin or spin.isAncestorOf(focus)):return
    if spin.value()!=value:spin.setValue(value)


def parameter_row(spin, label, slider=True, compact=False):
    """Retain the native numeric editor and pair it with a larger drag target."""
    spin.setButtonSymbols(QAbstractSpinBox.ButtonSymbols.NoButtons)
    spin.setKeyboardTracking(False)
    spin.setAccelerated(True)
    spin.setMinimumHeight(32)
    spin.setAccessibleName(label)
    row = QWidget()
    layout = QHBoxLayout(row)
    layout.setContentsMargins(0, 0, 0, 0)
    layout.setSpacing(4 if compact else 8)
    spin.setMinimumWidth(60 if compact else 80)
    if slider:
        scale = 10 if isinstance(spin, QDoubleSpinBox) else 1
        track = Slider()
        track.setAccessibleName(label + '滑杆')
        track.setRange(round(spin.minimum()*scale), round(spin.maximum()*scale))
        track.setSingleStep(max(1, round(spin.singleStep()*scale)))
        track.setValue(round(spin.value()*scale))
        def update_track(value):
            track.blockSignals(True)
            track.setValue(round(value*scale))
            track.blockSignals(False)
        spin.valueChanged.connect(update_track)
        track.valueChanged.connect(lambda value: spin.setValue(value/scale if scale != 1 else value))
        layout.addWidget(track, 1)
        spin.setFixedWidth(86)
        row.slider = track
    else:
        layout.addWidget(spin, 1)
        for text, direction in (('−', -1), ('+', 1)):
            button = QToolButton()
            button.setObjectName('stepper')
            button.setText(text)
            button.setFixedSize(26 if compact else 32, 32)
            button.setFocusPolicy(Qt.FocusPolicy.TabFocus)
            button.setAutoRepeat(True)
            button.setAutoRepeatDelay(350)
            button.setAutoRepeatInterval(65)
            button.setAccessibleName(label + ('减少' if direction < 0 else '增加'))
            def step(checked=False,d=direction):
                spin.interpretText();spin.stepBy(d);spin.setFocus(Qt.FocusReason.MouseFocusReason)
            button.clicked.connect(step)
            layout.addWidget(button)
    if slider:
        layout.addWidget(spin)
    row.enabled_state=_EnabledState(spin,row)
    return row


class Switch(QCheckBox):
    """Native checkbox semantics with a spacious switch presentation."""
    def sizeHint(self):
        return QSize(self.fontMetrics().horizontalAdvance(self.text())+62, 34)

    def hitButton(self, position):
        return self.rect().contains(position)

    def paintEvent(self, event):
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        if not self.isEnabled():
            p.setOpacity(0.4)
        y = (self.height()-22)/2
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(self.palette().color(QPalette.ColorRole.Highlight) if self.isChecked() else QColor('#858c96'))
        p.drawRoundedRect(0, y, 40, 22, 11, 11)
        p.setBrush(self.palette().color(QPalette.ColorRole.HighlightedText) if self.isChecked() else QColor('#ffffff'))
        p.drawEllipse(22 if self.isChecked() else 4, y+4, 14, 14)
        p.setPen(self.palette().color(QPalette.ColorRole.WindowText))
        p.drawText(self.rect().adjusted(52, 0, -2, 0), Qt.AlignmentFlag.AlignVCenter, self.text())
        if self.hasFocus():
            p.setPen(QPen(self.palette().color(QPalette.ColorRole.Highlight), 1, Qt.PenStyle.DotLine))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRoundedRect(self.rect().adjusted(1, 1, -1, -1), 5, 5)
        p.end()
