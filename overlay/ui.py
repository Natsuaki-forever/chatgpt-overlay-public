import ctypes
import os
import sys
import winreg
from pathlib import Path
from dataclasses import replace

from PySide6.QtCore import QAbstractNativeEventFilter, QObject, QPointF, QRectF, Qt, QTimer, Signal, QElapsedTimer, QSize, QRect, QEvent, QSettings
from PySide6.QtGui import QAction, QColor, QFont, QIcon, QPainter, QPen, QPixmap, QPalette
from PySide6.QtWidgets import (
    QApplication, QCheckBox, QColorDialog, QComboBox, QDialog, QDoubleSpinBox,
    QFormLayout, QHBoxLayout, QLabel, QMenu, QWidgetAction,
    QFileDialog, QMessageBox, QPlainTextEdit, QPushButton, QSpinBox, QStyle, QSystemTrayIcon, QTextEdit,
    QVBoxLayout, QWidget, QScrollArea, QStackedWidget, QToolButton, QButtonGroup, QSplitter, QGridLayout,
)

from . import native
from .model import Region, Store
from . import effects
from .session import EditSession
from .chrome import CloseButton, WindowChrome
from .region_list import RegionList
from .surfaces import SurfaceManager
from .widgets import SpinBox as QSpinBox, DoubleSpinBox as QDoubleSpinBox, Switch as QCheckBox, parameter_row, sync_number


def screen_for_window(app, hwnd):
    info = native.monitor_box(hwnd)
    device = native.monitor_device(hwnd).casefold()
    # Mixed-DPI desktops can make Qt's virtual coordinates differ from user32.
    # The Windows display device name is stable in both APIs.
    if device:
        for screen in app.screens():
            if screen.name().casefold() == device:
                return screen
    if info:
        x, y, _, _ = info[0]
        # Windows Qt leaves screen origins in native desktop coordinates while
        # scaling screen sizes. Do not multiply global x/y by a DPR.
        for screen in app.screens():
            origin = screen.geometry().topLeft()
            if origin.x() == x and origin.y() == y:
                return screen
    return app.primaryScreen()


def canvas_geometry(screen, physical_box, monitor_rect):
    x, y, width, height = physical_box
    origin_x, origin_y, monitor_width, monitor_height = monitor_rect
    logical = screen.geometry()
    # Preserve the relative location inside the matching monitor.  Applying a
    # DPR to global desktop coordinates is wrong when displays have mixed DPI.
    sx = logical.width() / monitor_width if monitor_width else 1
    sy = logical.height() / monitor_height if monitor_height else 1
    return QRect(round(logical.x()+(x-origin_x)*sx), round(logical.y()+(y-origin_y)*sy),
                 max(1, round(width*sx)), max(1, round(height*sy)))


class Canvas(QWidget):
    selected = Signal(int)
    changed = Signal()
    edit_requested = Signal(int)
    done = Signal()
    drag_finished = Signal()

    def __init__(self, regions):
        super().__init__(None, Qt.WindowType.Tool | Qt.WindowType.FramelessWindowHint)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.regions = regions
        self.region_origin = QPointF()
        self.editing = False
        self.animate_enabled = True
        self.index = -1
        self.start = None
        self.original = None
        self.mode = None
        self.draft = None
        self.picking = False
        self.pick_callback = None
        self.clock = QElapsedTimer()
        self.frames = 0
        self.clock.start()
        self.animation_timer = QTimer(self)
        self.animation_timer.setTimerType(Qt.TimerType.PreciseTimer)
        self.animation_timer.setInterval(33)
        self.animation_timer.timeout.connect(self.animate)

    def animate(self):
        for r in self.regions:
            if effects.animated(r):
                self.update(self.rect_for(r).toAlignedRect())

    def showEvent(self, event):
        super().showEvent(event)
        self.update()

    def hideEvent(self, event):
        self.animation_timer.stop()
        self.start = self.draft = None
        effects.release_buffers()
        super().hideEvent(event)

    def rect_for(self, region):
        return QRectF(region.x-self.region_origin.x(), region.y-self.region_origin.y(), region.width, region.height)

    def nativeEvent(self, event_type, message):
        msg = native.W.MSG.from_address(int(message))
        if not self.editing and msg.message == 0x0084:  # WM_NCHITTEST
            return True, -1  # HTTRANSPARENT; layered style handles cross-process input
        return False, 0

    def set_editing(self, value, owner):
        self.editing = value
        self.start = self.draft = None
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, not value)
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, not value)
        native.configure(int(self.winId()), owner, value)
        self.update()

    def paintEvent(self, event):
        self.frames += 1
        running = self.animate_enabled and self.isVisible() and any(effects.animated(r) for r in self.regions)
        if running and not self.animation_timer.isActive():
            self.animation_timer.start()
        elif not running:
            self.animation_timer.stop()
        p = QPainter(self)
        p.setCompositionMode(QPainter.CompositionMode.CompositionMode_Source)
        p.fillRect(self.rect(), Qt.GlobalColor.transparent)
        p.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceOver)
        p.setRenderHint(QPainter.RenderHint.TextAntialiasing)
        if self.editing:
            p.fillRect(self.rect(), QColor(35, 130, 120, 18))
        for i, r in enumerate(self.regions):
            rect = self.rect_for(r)
            bg = QColor(r.background)
            bg.setAlpha(r.opacity)
            p.fillRect(rect, bg)
            p.save()
            p.setClipRect(rect)
            font = QFont(r.font)
            font.setPixelSize(r.size)
            font.setBold(r.bold)
            font.setLetterSpacing(QFont.SpacingType.AbsoluteSpacing, r.letter_spacing)
            p.setFont(font)
            p.setPen(QColor(r.color))
            if not effects.draw(p, r, rect, self.clock.elapsed()/1000, self.devicePixelRatioF()):
                layout, height = effects.text_layout(r.text, font, max(1, rect.width()-2*r.padding), rect.height(), r.align, False)
                layout.draw(p, rect.topLeft()+QPointF(r.padding, (rect.height()-height)/2))
            p.restore()
            if self.editing:
                p.setPen(QPen(QColor("#8bdcc0" if i == self.index else "#a0aaa8"), 1.5 if i == self.index else 1))
                p.drawRect(rect)
                if i == self.index:
                    p.fillRect(QRectF(rect.right()-6, rect.bottom()-6, 12, 12), QColor("#2ab89d"))
        if self.draft:
            p.setPen(QPen(QColor("#2ab89d"), 2, Qt.PenStyle.DashLine))
            p.drawRect(self.draft)
        p.end()

    def mousePressEvent(self, event):
        if not self.editing or event.button() != Qt.MouseButton.LeftButton:
            return
        if self.picking:
            callback = self.pick_callback
            self.setCursor(Qt.CursorShape.ArrowCursor)
            self.hide()
            position = event.globalPosition().toPoint()
            QTimer.singleShot(120, lambda: callback(position))
            return
        self.start = event.position()
        self.index = -1
        for i in reversed(range(len(self.regions))):
            rect = self.rect_for(self.regions[i])
            if rect.adjusted(-6, -6, 6, 6).contains(self.start):
                self.index = i
                self.original = replace(self.regions[i])
                self.mode = "resize" if (self.start - rect.bottomRight()).manhattanLength() < 18 else "move"
                break
        if self.index < 0:
            self.mode = "create"
            self.draft = QRectF(self.start, self.start)
        self.selected.emit(self.index)
        self.update()

    def mouseMoveEvent(self, event):
        if self.start is None:
            return
        point = QPointF(max(0, min(self.width(), event.position().x())), max(0, min(self.height(), event.position().y())))
        if self.mode == "create":
            self.draft = QRectF(self.start, point).normalized()
        else:
            r, old = self.regions[self.index], self.original
            delta = point - self.start
            if self.mode == "move":
                r.x = max(0, min(self.width()-r.width, old.x + delta.x()))
                r.y = max(0, min(self.height()-r.height, old.y + delta.y()))
            else:
                r.width = max(8, min(self.width()-r.x, old.width + delta.x()))
                r.height = max(8, min(self.height()-r.y, old.height + delta.y()))
        self.update()

    def mouseReleaseEvent(self, event):
        if self.start is None:
            return
        if self.mode == "create" and self.draft and self.draft.width() >= 8 and self.draft.height() >= 8:
            q = self.draft
            self.regions.append(Region(x=q.x(), y=q.y(), width=q.width(), height=q.height()))
            self.index = len(self.regions)-1
            self.selected.emit(self.index)
        self.start = self.draft = None
        self.changed.emit()
        self.drag_finished.emit()
        self.update()

    def mouseDoubleClickEvent(self, event):
        if self.editing and self.index >= 0:
            self.edit_requested.emit(self.index)

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape:
            self.done.emit()
        elif event.key() == Qt.Key.Key_Delete and self.index >= 0:
            self.regions.pop(self.index)
            self.index = -1
            self.changed.emit()
            self.selected.emit(-1)
            self.update()
        elif self.index >= 0 and event.key() in (Qt.Key.Key_Left, Qt.Key.Key_Right, Qt.Key.Key_Up, Qt.Key.Key_Down):
            r = self.regions[self.index]
            step = 10 if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else 1
            r.x = max(0, r.x + step * ((event.key() == Qt.Key.Key_Right) - (event.key() == Qt.Key.Key_Left)))
            r.y = max(0, r.y + step * ((event.key() == Qt.Key.Key_Down) - (event.key() == Qt.Key.Key_Up)))
            self.changed.emit()
            self.selected.emit(self.index)
            self.update()


class Inspector(QDialog):
    def showEvent(self, event):
        native.dark_frame(int(self.winId()))
        super().showEvent(event)

    def changeEvent(self, event):
        super().changeEvent(event)
        if event.type() in (QEvent.Type.ActivationChange, QEvent.Type.PaletteChange):
            native.dark_frame(int(self.winId()))

    def __init__(self, controller, parent=None):
        # Give Qt the same owner chain as Win32 before the first show/activation.
        super().__init__(parent, Qt.WindowType.Tool | Qt.WindowType.FramelessWindowHint)
        self.setObjectName('inspector')
        self.c = controller
        self.loading = False
        self.setWindowTitle("ChatGPT Overlay · 编辑区域")
        self.setMinimumSize(640, 420)
        self.resize(920, 640)
        self._displayed_region=None
        from .editor_theme import editor_font
        self.setFont(editor_font())
        self.theme_settings=QSettings('ChatGPTOverlay','Editor')
        self.theme_mode=self.theme_settings.value('theme','system')
        if self.theme_mode not in ('system','light','dark'):self.theme_mode='system'
        self._feedback_level='info';self._save_error=False
        self.c.app.styleHints().colorSchemeChanged.connect(self.system_theme_changed)
        outer = QVBoxLayout(self)
        outer.setContentsMargins(16, 12, 16, 12)
        outer.setSpacing(10)
        self.title_bar = QWidget(self)
        header = QHBoxLayout(self.title_bar)
        header.setContentsMargins(0, 0, 0, 0)
        header.setSpacing(6)
        title = QLabel('Overlay')
        title.setObjectName('appTitle')
        header.addWidget(title)
        header.addStretch()
        self.theme_button = QToolButton(self.title_bar)
        self.theme_button.setObjectName('iconControl')
        self.theme_button.setFixedSize(32, 32)
        self.theme_button.setIconSize(QSize(18, 18))
        self.theme_button.setAccessibleName('界面主题')
        self.theme_button.setFocusPolicy(Qt.FocusPolicy.TabFocus)
        self.theme_button.installEventFilter(self)
        self.theme_button.setPopupMode(QToolButton.ToolButtonPopupMode.InstantPopup)
        theme_menu = QMenu(self.theme_button)
        self.theme_actions = {}
        for label, value in (('跟随系统', 'system'), ('浅色', 'light'), ('深色', 'dark')):
            action = theme_menu.addAction(label)
            action.setCheckable(True)
            action.triggered.connect(lambda checked=False, mode=value: self.set_theme(mode))
            self.theme_actions[value] = action
        self.theme_button.setMenu(theme_menu)
        header.addWidget(self.theme_button)
        status = QLabel('Ctrl+Shift+E 打开面板',self.title_bar)
        status.setObjectName('hint')
        self.shortcut_hint=status
        status.hide()
        self.close_button = CloseButton(self.title_bar)
        self.close_button.clicked.connect(self.close)
        header.addWidget(self.close_button)
        outer.addWidget(self.title_bar)
        connection = self.connection_button = QToolButton(self.title_bar)
        connection.setObjectName('iconControl')
        connection.setFixedSize(32, 32)
        connection.setIconSize(QSize(18, 18))
        connection.setAccessibleName('应用设置')
        connection.setToolTip('应用设置 · 附着窗口和面板显示器')
        connection.setCheckable(True)
        header.insertWidget(header.count()-1,connection)
        header.insertSpacing(header.count()-1, 6)
        connection_panel = QWidget()
        connection_layout = QVBoxLayout(connection_panel)
        connection_layout.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(connection_panel)
        connection_panel.hide()
        connection.toggled.connect(connection_panel.setVisible)
        targets = QHBoxLayout()
        targets.addWidget(QLabel('附着窗口'))
        self.target_choice = QComboBox()
        self.target_choice.setMinimumContentsLength(18)
        targets.addWidget(self.target_choice, 1)
        connection_layout.addLayout(targets)
        self.target_choice.activated.connect(self.target_selected)
        displays = QHBoxLayout()
        displays.addWidget(QLabel('面板显示屏'))
        self.display_choice = QComboBox()
        self.display_choice.setMinimumContentsLength(12)
        self.display_choice.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        displays.addWidget(self.display_choice, 1)
        follow = QPushButton('移到目标窗口所在屏')
        follow.clicked.connect(self.move_to_target)
        displays.addWidget(follow)
        connection_layout.addLayout(displays)
        if hasattr(controller, 'issues'):
            diagnostics = QPushButton('问题记录…')
            diagnostics.clicked.connect(self.show_issue_history)
            connection_layout.addWidget(diagnostics, alignment=Qt.AlignmentFlag.AlignLeft)
        self.display_choice.activated.connect(self.display_selected)
        controller.app.screenAdded.connect(self.refresh_displays)
        controller.app.screenRemoved.connect(self.refresh_displays)
        self.refresh_displays()
        self.connection_status=QLabel('等待连接')
        self.connection_status.setObjectName('connectionStatus');header.insertWidget(1,self.connection_status)
        regions_panel=QWidget();regions_layout=QVBoxLayout(regions_panel)
        regions_layout.setContentsMargins(0,0,0,0);regions_layout.setSpacing(8)
        self.regions_heading=QLabel('覆盖区域');self.regions_heading.setObjectName('hint')
        regions_layout.addWidget(self.regions_heading)
        self.items = RegionList()
        self.items.setIconSize(QSize(12, 12))
        self.items.setMinimumHeight(160)
        self.items.setSpacing(3)
        self.items.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        regions_layout.addWidget(self.items,1)
        self.add_button=QPushButton('＋ 新建区域');self.add_button.clicked.connect(self.add_region)
        regions_layout.addWidget(self.add_button)
        self.free_region_button = QPushButton('全窗口框选')
        self.free_region_button.clicked.connect(lambda: self.c.set_canvas_mode(
            'idle' if self.c.canvas_mode == 'free' else 'free'))
        self.free_region_button.hide()
        regions_layout.addWidget(self.free_region_button)
        self.items.menu_requested.connect(self.show_region_menu)
        self.items.currentRowChanged.connect(self.choose)
        self.hint = QLabel("先在目标窗口中拖动，创建并选中一个矩形。")
        self.hint.setObjectName('feedback')
        self.hint.setWordWrap(True)
        self.hint.setMinimumHeight(32)
        self.hint.setMaximumHeight(48)
        self.hint.setTextInteractionFlags(Qt.TextInteractionFlag.TextSelectableByMouse)
        body = QHBoxLayout()
        body.setSpacing(8)
        outer.addLayout(body, 1)
        properties=QWidget()
        properties_layout=QVBoxLayout(properties)
        properties_layout.setContentsMargins(16,0,0,0)
        properties_layout.setSpacing(12)
        self.region_title=QLabel('选择覆盖区域')
        self.region_title.setObjectName('sectionTitle')
        property_header = QHBoxLayout()
        property_header.setSpacing(8)
        properties_layout.addLayout(property_header)
        nav=QHBoxLayout()
        nav.setContentsMargins(0,0,0,0);nav.setSpacing(8)
        properties_layout.addLayout(nav)
        self.pages = QStackedWidget()
        self.workspace=QSplitter(Qt.Orientation.Horizontal)
        regions_panel.setMinimumWidth(160);self.pages.setMinimumWidth(300)
        regions_panel.setMaximumWidth(240)
        properties_layout.addWidget(self.pages,1)
        self.workspace.addWidget(regions_panel);self.workspace.addWidget(properties)
        self.workspace.setStretchFactor(0,0);self.workspace.setStretchFactor(1,1)
        self.workspace.setSizes([204,660]);self.regions_panel=regions_panel
        self.regions_toggle = QToolButton(properties)
        self.regions_toggle.setObjectName('iconControl')
        self.regions_toggle.setFixedSize(32, 32)
        self.regions_toggle.setIconSize(QSize(18, 18))
        self.regions_toggle.setAccessibleName('区域列表')
        self.regions_toggle.setCheckable(True)
        self.regions_toggle.toggled.connect(self.update_regions_tooltip)
        self.regions_toggle.setChecked(True)
        self._regions_manually_toggled=False
        self.regions_toggle.clicked.connect(self.toggle_regions)
        property_header.addWidget(self.regions_toggle)
        property_header.addWidget(self.region_title, 1)
        body.addWidget(self.workspace,1)
        self.categories = QButtonGroup(self)
        self.categories.setExclusive(True)
        forms = {}
        for index, (key, title, description) in enumerate((
            ('text', '文字', '内容、字体与对齐'), ('position', '位置', '精确调整矩形的坐标与尺寸'),
            ('background', '背景', '选择遮盖颜色与不透明度'), ('scroll', '滚动', '单行文字连续循环'),
            ('outline', '描边', '强调文字轮廓'), ('glow', '发光', '柔和光晕与呼吸动画'),
        )):
            button = QToolButton()
            button.setText(title)
            # Text tabs keep category navigation distinct from region selection.
            button.setObjectName('category')
            button.setIconSize(QSize(18, 18))
            button.setToolButtonStyle(Qt.ToolButtonStyle.ToolButtonTextBesideIcon)
            button.setCheckable(True)
            button.setFixedHeight(36)
            button.setToolTip(title + ' · ' + description)
            button.setAccessibleName(title + '设置')
            self.categories.addButton(button, index)
            if index<4:
                button.setText('效果' if index==3 else title)
                nav.addWidget(button)
            else:button.hide()
            scroll = QScrollArea()
            scroll.setWidgetResizable(True)
            scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
            page = QWidget()
            page.setObjectName('propertyPage')
            content = QVBoxLayout(page)
            content.setContentsMargins(0, 6, 4, 12)
            content.setSpacing(6)
            heading = QLabel(title)
            heading.setObjectName('sectionTitle')
            if index>=3:content.addWidget(heading)
            subtitle = QLabel(description)
            subtitle.setObjectName('sectionDescription')
            subtitle.setWordWrap(True)
            if index>=3:content.addWidget(subtitle)
            form = QFormLayout()
            form.setVerticalSpacing(10)
            form.setRowWrapPolicy(QFormLayout.RowWrapPolicy.WrapLongRows)
            form.setFieldGrowthPolicy(QFormLayout.FieldGrowthPolicy.AllNonFixedFieldsGrow)
            content.addSpacing(8)
            content.addLayout(form)
            content.addStretch()
            scroll.setWidget(page)
            self.pages.addWidget(scroll)
            forms[key] = form
        nav.addStretch()
        self.effect_tabs=QWidget()
        effect_nav=QHBoxLayout(self.effect_tabs);effect_nav.setContentsMargins(0,0,0,0)
        self.effect_group=QButtonGroup(self)
        for i,title in enumerate(('滚动','描边','发光'),3):
            tab=QToolButton();tab.setText(title);tab.setCheckable(True)
            self.effect_group.addButton(tab,i);effect_nav.addWidget(tab)
        effect_nav.addStretch();self.effect_group.button(3).setChecked(True)
        properties_layout.insertWidget(2,self.effect_tabs);self.effect_tabs.hide()
        self.effect_group.idClicked.connect(self.pages.setCurrentIndex)
        self.categories.idClicked.connect(self.pages.setCurrentIndex)
        self.pages.currentChanged.connect(self.sync_category)

        self.categories.button(0).setChecked(True)
        self.pages.setEnabled(False)
        form = forms['text']
        self.text = QTextEdit()
        self.text.setFixedHeight(72)
        self.text.setAcceptRichText(False)
        form.addRow("文字", self.text)
        self.text.textChanged.connect(self.apply)
        from .widgets import FontPicker
        self.font = FontPicker(defer_families=True)
        self.font.setMinimumContentsLength(12)
        self.font.setSizeAdjustPolicy(QComboBox.SizeAdjustPolicy.AdjustToMinimumContentsLengthWithIcon)
        form.addRow("字体", self.font)
        self.font.currentFontChanged.connect(self.apply)
        geometry_box = QWidget()
        geometry_form = QGridLayout(geometry_box)
        geometry_form.setContentsMargins(0, 0, 0, 8)
        geometry_form.setVerticalSpacing(10);geometry_form.setHorizontalSpacing(8)
        geometry_form.setColumnStretch(1,1);geometry_form.setColumnStretch(3,1)
        geometry_box.setToolTip('坐标相对于目标客户区左上角，位置与尺寸单位为DIP。')
        forms['position'].addRow(geometry_box)
        self.anchor_controls = {}
        self.position_form=forms['position']
        if getattr(self.c, 'supports_anchors', False):
            for key, label, choices in (
                ('anchor_x', '横向定位', (('left', '跟随左侧'), ('center', '跟随中心'), ('right', '跟随右侧'))),
                ('anchor_y', '纵向定位', (('top', '跟随顶部'), ('center', '跟随中心'), ('bottom', '跟随底部'))),
            ):
                combo = QComboBox()
                for value, title in choices:
                    combo.addItem(title, value)
                combo.currentIndexChanged.connect(self.apply)
                forms['position'].addRow(label, combo)
                self.anchor_controls[key] = combo
            for combo in self.anchor_controls.values():combo.setToolTip('切换锚定方向时保持当前位置；只移动整个窗口时直接平移。')
        self.visual_buttons = {}
        self.visual_controls = None
        if hasattr(self.c, 'visual_action'):
            from .visual_controls import VisualControls
            self.visual_controls=VisualControls(self.c)
            forms['position'].insertRow(0,self.visual_controls)
            self.visual_controls.kind.setToolTip('文字／图案只跟位置；完整边框可跟随四边。视觉定位为可选模式，校准失败保留原设置。')
            for action, label in (('calibrate', '用当前遮盖框校准'), ('retry', '重试识别组件'), ('anchor', '改回窗口锚定')):
                button = QPushButton(label)
                if action=='anchor':button.setToolTip('改回窗口锚定；暂时没有确认位置时使用保存的坐标。')
                button.clicked.connect(lambda checked=False, action=action: self.c.visual_action(action))
                self.visual_controls.advanced.body.layout().addWidget(button)
                self.visual_buttons[action] = button
        self.controls = {}
        for key, label, minimum, maximum in (("size", "字号 (px)", 6, 200), ("x", "X", 0, 100000), ("y", "Y", 0, 100000), ("width", "宽度", 8, 100000), ("height", "高度", 8, 100000), ("padding", "文字边距", 0, 100), ("opacity", "背景不透明度", 0, 255)):
            spin = QDoubleSpinBox() if key in ("x", "y", "width", "height") else QSpinBox()
            spin.setRange(minimum, maximum)
            spin.setSingleStep(1)
            if key == "opacity":
                spin.setToolTip("0 完全透明，255 完全不透明")
            if key in ('x','y','width','height'):
                index=('x','y','width','height').index(key);row,column=divmod(index,2)
                geometry_form.addWidget(QLabel(label),row,column*2)
                geometry_form.addWidget(parameter_row(spin,label,slider=False,compact=True),row,column*2+1)
            else:
                target_form=forms['background'] if key=='opacity' else form
                target_form.addRow(label,parameter_row(spin,label,slider=key=='opacity'))
            spin.valueChanged.connect(self.apply)
            self.controls[key] = spin
        self.controls['size'].setSuffix(' px')
        self.bold = QCheckBox("粗体")
        spacing = QDoubleSpinBox()
        spacing.setRange(-10, 50)
        spacing.setSingleStep(0.5)
        spacing.setSuffix(' px')
        spacing.setToolTip('0 为默认字距；负值收紧，正值拉开。')
        spacing.valueChanged.connect(self.apply)
        self.controls['letter_spacing'] = spacing
        form.addRow('字距', parameter_row(spacing, '字距',slider=False))
        self.bold.toggled.connect(self.apply)
        self.align = QComboBox()
        self.align.addItems(["左对齐", "居中", "右对齐"])
        typography=QWidget();typography_row=QHBoxLayout(typography);typography_row.setContentsMargins(0,0,0,0)
        self.bold.hide();self.align.hide()
        weight_group=QButtonGroup(self)
        for value,title in ((False,'常规'),(True,'粗体')):
            button=QToolButton();button.setText(title);button.setCheckable(True)
            button.setObjectName('segment');button.setFixedWidth(56);button.setChecked(self.bold.isChecked()==value)
            weight_group.addButton(button,int(value));typography_row.addWidget(button)
        weight_group.idClicked.connect(lambda value:self.bold.setChecked(bool(value)))
        self.bold.toggled.connect(lambda value:weight_group.button(int(value)).setChecked(True))
        typography_row.addSpacing(16)
        align_group=QButtonGroup(self)
        self.alignment_buttons=[]
        for index,title in enumerate(('左对齐','居中','右对齐')):
            button=QToolButton();button.setObjectName('segment');button.setFixedWidth(40);button.setToolTip(title);button.setAccessibleName(title)
            pixmap=QPixmap(24,24);pixmap.fill(Qt.GlobalColor.transparent)
            painter=QPainter(pixmap);painter.setPen(QPen(QColor('#c7d0dc'),1.5))
            for row,length in enumerate((18,12,18,12)):
                x=3+(0 if index==0 else (18-length)/2 if index==1 else 18-length)
                painter.drawLine(QPointF(x,5+row*4),QPointF(x+length,5+row*4))
            painter.end();button.setIcon(QIcon(pixmap));button.setCheckable(True)
            self.alignment_buttons.append(button)
            button.setChecked(index==self.align.currentIndex());align_group.addButton(button,index);typography_row.addWidget(button)
        align_group.idClicked.connect(self.align.setCurrentIndex)
        self.align.currentIndexChanged.connect(lambda value:align_group.button(value).setChecked(True))
        typography_row.addStretch()
        form.addRow('排版',typography)
        self.align.currentIndexChanged.connect(self.apply)
        self.color_buttons={}
        color_rows = {}
        for key, label in (("color", "文字颜色"), ("background", "背景颜色")):
            button = QPushButton(label)
            button.setObjectName('colorButton');button.setToolTip('点击选择'+label)
            self.color_buttons[key]=button
            button.clicked.connect(lambda checked=False, key=key: self.color(key))
            pick = QPushButton('取色')
            pick.setToolTip('从目标窗口取色')
            pick.clicked.connect(lambda checked=False, key=key: self.c.pick_color(key))
            row = QWidget()
            layout = QHBoxLayout(row)
            layout.setContentsMargins(0, 0, 0, 0)
            layout.addWidget(button, 1)
            layout.addWidget(pick)
            color_rows[key] = row
            forms['text' if key == 'color' else 'background'].addRow(label, row)
        # Reparent existing controls, retaining their validated edit/commit paths.
        retired=[]
        while form.rowCount():
            taken=form.takeRow(0)
            for item in (taken.labelItem,taken.fieldItem):
                if item and item.widget():
                    item.widget().hide();retired.append(item.widget())
        text_grid=QGridLayout();text_grid.setHorizontalSpacing(16);text_grid.setVerticalSpacing(10)
        text_grid.setColumnStretch(0,2);text_grid.setColumnStretch(1,1)
        text_grid.addWidget(QLabel('内容'),0,0)
        text_grid.addWidget(self.text,1,0,1,2)
        text_grid.addWidget(QLabel('字体'),2,0);text_grid.addWidget(QLabel('字号'),2,1)
        text_grid.addWidget(self.font,3,0)
        text_grid.addWidget(parameter_row(self.controls['size'],'字号',slider=False,compact=True),3,1)
        text_grid.addWidget(QLabel('排版'),4,0);text_grid.addWidget(QLabel('文字颜色'),4,1)
        text_grid.addWidget(typography,5,0);text_grid.addWidget(color_rows['color'],5,1)
        text_grid.addWidget(QLabel('字距'),6,0);text_grid.addWidget(QLabel('文字边距'),6,1)
        text_grid.addWidget(parameter_row(spacing,'字距',slider=False,compact=True),7,0)
        text_grid.addWidget(parameter_row(self.controls['padding'],'文字边距',slider=False,compact=True),7,1)
        form.addRow(text_grid)
        kept=(self.text,self.font,typography,color_rows['color'])
        for widget in kept:widget.show()
        for widget in retired:
            if widget not in kept:widget.deleteLater()
        self.effect_checks = {}
        for key, label in (("scroll", "循环滚动"), ("outline", "文字描边"), ("glow", "文字发光"), ("glow_pulse", "发光呼吸动画")):
            check = QCheckBox(label)
            check.toggled.connect(self.apply)
            forms['glow' if key == 'glow_pulse' else key].addRow(check)
            self.effect_checks[key] = check
        self.direction = QComboBox()
        self.direction.addItems(["向左", "向右"])
        self.direction.currentIndexChanged.connect(self.apply)
        forms['scroll'].addRow("滚动方向", self.direction)
        feather = QCheckBox('两侧渐隐 / 渐现')
        feather.setToolTip('柔化滚动文字进出边缘；背景保持不变。')
        feather.toggled.connect(self.apply)
        forms['scroll'].addRow('边缘羽化', feather)
        self.effect_checks['scroll_feather'] = feather
        feather_width = QSpinBox()
        feather_width.setRange(1, 200)
        feather_width.setSuffix(' px')
        feather_width.valueChanged.connect(self.apply)
        forms['scroll'].addRow('羽化宽度', parameter_row(feather_width, '羽化宽度'))
        self.controls['scroll_feather_width'] = feather_width
        for key, label, low, high in (("scroll_speed", "速度 (px/秒)", 5, 300), ("scroll_gap", "循环间距", 8, 300), ("outline_width", "描边宽度", 0.5, 12), ("glow_radius", "发光半径", 1, 24), ("glow_strength", "发光强度 (%)", 0, 100), ("glow_period", "呼吸周期 (秒)", 0.5, 10)):
            spin = QDoubleSpinBox() if key in ("outline_width", "glow_period") else QSpinBox()
            spin.setRange(low, high)
            spin.setSingleStep(0.5 if isinstance(spin, QDoubleSpinBox) else 1)
            spin.valueChanged.connect(self.apply)
            forms[key.split('_')[0]].addRow(label, parameter_row(spin, label))
            self.controls[key] = spin
        for key, label in (("outline_color", "描边颜色"), ("glow_color", "发光颜色")):
            button = QPushButton(label)
            button.clicked.connect(lambda checked=False, key=key: self.color(key))
            forms[key.split('_')[0]].addRow(label, button)
        row = QHBoxLayout()
        finish = QPushButton("完成")
        finish.setToolTip('保存并退出编辑（Esc）');finish.setMinimumWidth(80)
        finish.setObjectName('finish')
        finish.clicked.connect(lambda: self.c.set_editing(False))
        self.save_status=QLabel('已保存');self.save_status.setObjectName('hint');row.addWidget(self.save_status)
        row.addStretch()
        row.addWidget(finish)
        row.insertWidget(1,self.hint,1)
        outer.addLayout(row)
        self.chrome = WindowChrome(self, self.title_bar, self.close_button)
        self.apply_theme()
        # Qt's Windows font engine exposes more families after the controls are styled.
        self.font.load_families()
        # Keep the first toolbar button from receiving focus when the panel opens.
        self.setFocus(Qt.FocusReason.OtherFocusReason)

    def eventFilter(self, watched, event):
        if (watched is self.theme_button
                and event.type() == QEvent.Type.MouseButtonPress
                and event.button() == Qt.MouseButton.LeftButton
                and self.theme_button.hasFocus()):
            self.setFocus(Qt.FocusReason.OtherFocusReason)
        return super().eventFilter(watched, event)

    def system_theme_changed(self,*args):
        if self.theme_mode=='system':self.apply_theme()

    def set_theme(self,mode,persist=True):
        if mode not in ('system','light','dark'):return
        self.theme_mode=mode
        if persist:self.theme_settings.setValue('theme',mode)
        self.apply_theme()

    def apply_theme(self):
        from .editor_theme import DARK,LIGHT,theme_style,apply_text_rendering,toolbar_icon
        if hasattr(self, 'theme_actions'):
            for value, action in self.theme_actions.items():
                action.setChecked(value == self.theme_mode)
            self.theme_button.setToolTip(f'界面主题 · {self.theme_actions[self.theme_mode].text()}')
        mode=self.theme_mode
        if mode=='system':mode='dark' if self.c.app.styleHints().colorScheme()==Qt.ColorScheme.Dark else 'light'
        self.resolved_theme=mode
        colors=LIGHT if mode=='light' else DARK
        self.theme_colors=colors
        if hasattr(self, 'regions_toggle'):
            for button, icon in ((self.theme_button, self.theme_mode),
                                 (self.connection_button, 'settings'),
                                 (self.regions_toggle, 'regions')):
                button.setIcon(toolbar_icon(icon, colors['text']))
        palette=self.palette()
        for role,key in ((QPalette.ColorRole.Window,'window'),(QPalette.ColorRole.WindowText,'text'),
                         (QPalette.ColorRole.Text,'text'),(QPalette.ColorRole.ButtonText,'text'),
                         (QPalette.ColorRole.Highlight,'accent')):
            palette.setColor(role,QColor(colors[key]))
        palette.setColor(QPalette.ColorRole.HighlightedText,QColor('#ffffff' if mode=='light' else '#172c3e'))
        self.setPalette(palette);self.setStyleSheet(theme_style(mode))
        apply_text_rendering(self)
        for index,button in enumerate(getattr(self,'alignment_buttons',[])):
            pixmap=QPixmap(24,24);pixmap.fill(Qt.GlobalColor.transparent)
            painter=QPainter(pixmap);painter.setPen(QPen(QColor(colors['text']),1.5))
            for row,length in enumerate((18,12,18,12)):
                x=3+(0 if index==0 else (18-length)/2 if index==1 else 18-length)
                painter.drawLine(QPointF(x,5+row*4),QPointF(x+length,5+row*4))
            painter.end();button.setIcon(QIcon(pixmap))
        if hasattr(self,'hint'):
            self.hint.setStyleSheet('color: '+colors[self._feedback_level]+';' if self._feedback_level in ('error','success') else '')
        if hasattr(self,'save_status'):self.save_status.setStyleSheet('color: '+colors['error']+';' if self._save_error else '')

    def sync_category(self,index):
        self.categories.button(min(index,3)).setChecked(True)
        self.effect_tabs.setVisible(index>=3)
        if index>=3:self.effect_group.button(index).setChecked(True)

    def show_region_menu(self, row, position):
        if not 0 <= row < len(self.c.regions) or getattr(self.c, 'calibration_preview', None):
            return
        region = self.c.regions[row]
        region_id = region.id
        name = (region.text.strip().splitlines() or ['空白遮盖'])[0]
        if len(name) > 28:
            name = name[:28] + '…'
        menu=QMenu(self)
        remove = menu.addAction('删除区域…')
        chosen = menu.exec(position)
        anchor = menu.pos()
        menu.deleteLater()
        if chosen == remove and self.isVisible():
            self.confirm_region_delete(region_id, name, anchor)

    def show_issue_history(self):
        store = self.c.issues
        dialog = QDialog(self)
        dialog.setWindowTitle('问题记录')
        dialog.resize(590, 410)
        layout = QVBoxLayout(dialog)
        layout.addWidget(QLabel('最近 100 条本地记录。导出内容包含问题编号和处理建议。'))
        report = QPlainTextEdit()
        report.setReadOnly(True)
        layout.addWidget(report)

        def refresh():
            report.setPlainText(store.report() if store else '当前主程序尚未提供问题记录。')

        def export():
            filename, _ = QFileDialog.getSaveFileName(dialog, '导出问题记录', 'Overlay-问题记录.txt', '文本文件 (*.txt)')
            if filename:
                try:
                    Path(filename).write_text(report.toPlainText(), encoding='utf-8')
                except OSError:
                    QMessageBox.warning(dialog, '导出失败', '请选择可写入的文件夹，再试一次。')

        actions = QHBoxLayout()
        actions.addStretch()
        for title, callback in (('刷新', refresh), ('导出…', export), ('关闭', dialog.accept)):
            button = QPushButton(title)
            button.clicked.connect(callback)
            actions.addWidget(button)
        layout.addLayout(actions)
        refresh()
        dialog.exec()
        dialog.deleteLater()

    def confirm_region_delete(self, region_id, name, position):
        menu = QMenu(self)
        menu.setAccessibleName('确认删除区域')
        content = QWidget()
        layout = QVBoxLayout(content)
        label = QLabel(f'删除「{name}」？')
        label.setTextFormat(Qt.TextFormat.PlainText)
        layout.addWidget(label)
        actions = QHBoxLayout()
        actions.addStretch()
        cancel = QPushButton('取消')
        remove = QPushButton('删除')
        remove.setObjectName('delete')
        actions.addWidget(cancel)
        actions.addWidget(remove)
        layout.addLayout(actions)
        action = QWidgetAction(menu)
        action.setDefaultWidget(content)
        menu.addAction(action)
        cancel.clicked.connect(menu.close)
        remove.clicked.connect(menu.close)
        remove.clicked.connect(lambda: self.delete_region_by_id(region_id))
        QTimer.singleShot(0, cancel.setFocus)
        menu.exec(position)
        menu.deleteLater()

    def toggle_regions(self,visible):
        self._regions_manually_toggled=True
        self.regions_panel.setVisible(visible)

    def update_regions_tooltip(self, expanded):
        self.regions_toggle.setToolTip('收起区域列表' if expanded else '展开区域列表')

    def resizeEvent(self,event):
        super().resizeEvent(event)
        if hasattr(self,'regions_toggle'):
            if not self._regions_manually_toggled:
                expanded=self.width()>=760
                self.regions_panel.setVisible(expanded);self.regions_toggle.setChecked(expanded)

    def add_region(self):
        self.commit()
        if hasattr(self.c,'create_region'):self.c.create_region()
        else:self.set_feedback('在目标窗口空白处拖动，即可创建新区域。')

    def set_feedback(self,text,level='info'):
        self.hint.setText(text)
        self.hint.setToolTip(text)
        self._feedback_level=level
        self.hint.setStyleSheet('color: '+self.theme_colors[level]+';' if level in ('error','success') else '')

    def set_save_status(self, revision, persisted, error=False, *, awaiting=False, rejected=False):
        if error:
            status = '保存失败'
        elif rejected:
            status = '操作未提交'
        elif awaiting:
            status = '提交中…'
        elif revision != persisted:
            status = '保存中…'
        else:
            status = '已保存'
        self.save_status.setText(status)
        self._save_error = bool(error or rejected)
        self.save_status.setStyleSheet('color: '+self.theme_colors['error']+';' if self._save_error else '')

    def refresh_displays(self, *args):
        self.display_choice.clear()
        for i, screen in enumerate(self.c.app.screens()):
            self.display_choice.addItem(f'{i+1} · {screen.name()}', screen)

    def display_selected(self, index):
        screen = self.display_choice.itemData(index)
        if screen in self.c.app.screens():
            self.move_to_screen(screen)

    def refresh_targets(self):
        if self.target_choice.view().isVisible():return
        current = self.c.target
        profile_managed = bool(getattr(self.c, 'canvas_modes', False) or getattr(self.c, 'application_id', ''))
        self.target_choice.blockSignals(True)
        self.target_choice.clear()
        self.target_choice.setEnabled(not profile_managed)
        self.target_choice.setToolTip('从托盘选择要绑定的窗口。' if profile_managed else '')
        candidates = ([current] if current else []) if profile_managed else native.targets()
        for index, hwnd in enumerate(candidates):
            box = native.client_box(hwnd)
            device = native.monitor_device(hwnd) or 'unknown display'
            size = f'{box[2]}x{box[3]}' if box else 'unavailable'
            self.target_choice.addItem(f'{index + 1} · {device} · {size}', hwnd)
            if hwnd == current:
                self.target_choice.setCurrentIndex(index)
        self.target_choice.blockSignals(False)

    def target_selected(self, index):
        hwnd = self.target_choice.itemData(index)
        if hwnd and hwnd != self.c.target:
            self.c.select_target(hwnd)

    def move_to_target(self):
        self.move_to_screen(screen_for_window(self.c.app, self.c.target))

    def move_to_screen(self, screen):
        # Set the Qt screen first, then use that screen's local logical coords.
        # This also works on displays left/above the primary screen.
        self.winId()
        self.windowHandle().setScreen(screen)
        area = screen.availableGeometry()
        width=min(880,max(1,area.width()-40));height=min(620,max(1,area.height()-48))
        self.setMinimumSize(min(640,width),min(420,height));self.resize(width,height)
        self.move(area.left()+max(0, (area.width()-self.width())//2), area.top()+24)
        for i in range(self.display_choice.count()):
            if self.display_choice.itemData(i) is screen:
                self.display_choice.setCurrentIndex(i)
                break

    @staticmethod
    def category_icon(index):
        pix = QPixmap(44, 44)
        pix.fill(Qt.GlobalColor.transparent)
        p = QPainter(pix)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(QPen(QColor('#c4d7e8'), 2.5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        if index in (0, 4):
            font = QFont('Segoe UI', 22, QFont.Weight.DemiBold)
            p.setFont(font)
            p.drawText(pix.rect(), Qt.AlignmentFlag.AlignCenter, 'T' if index == 0 else 'A')
            if index == 4:
                p.drawLine(10, 39, 34, 39)
        elif index == 1:
            p.drawRect(10, 10, 24, 24)
            p.drawLine(22, 4, 22, 14)
            p.drawLine(22, 30, 22, 40)
            p.drawLine(4, 22, 14, 22)
            p.drawLine(30, 22, 40, 22)
        elif index == 2:
            p.setBrush(QColor('#528574'))
            p.drawRoundedRect(8, 9, 28, 26, 4, 4)
            p.drawLine(12, 30, 31, 14)
        elif index == 3:
            for y, left in ((15, False), (30, True)):
                p.drawLine(7, y, 37, y)
                x, dx = (7, 6) if left else (37, -6)
                p.drawLine(x, y, x+dx, y-5)
                p.drawLine(x, y, x+dx, y+5)
        else:
            p.drawEllipse(15, 15, 14, 14)
            for x1, y1, x2, y2 in ((22, 4, 22, 9), (22, 35, 22, 40), (4, 22, 9, 22), (35, 22, 40, 22), (9, 9, 12, 12), (32, 32, 35, 35), (9, 35, 12, 32), (32, 12, 35, 9)):
                p.drawLine(x1, y1, x2, y2)
        p.end()
        return QIcon(pix)

    def refresh(self, index=None):
        self.refresh_targets()
        self.regions_heading.setText(f'覆盖区域 · {len(self.c.regions)}')
        self.loading = True
        while self.items.count()>len(self.c.regions):self.items.takeItem(self.items.count()-1)
        for i, r in enumerate(self.c.regions):
            status=getattr(self.c,'tracking_states',{}).get(r.id,{}).get('state')
            mode='窗口锚定' if r.position_mode=='anchor' else '文字定位' if (r.visual_binding or {}).get('tracking_kind')=='features' else '边框定位'
            short={'NEEDS_MANUAL':'需重选','UNAVAILABLE':'不可用','PAUSED':'暂停','HIDDEN':'隐藏','VERIFYING':'找回中','FOLLOWING':'已定位','HOLDING':'保持位置'}.get(status,'')
            name=r.text.replace('\n',' ')[:16] or '空白遮盖'
            label=f'{i+1:02d}  {name}\n{mode}'+(' · '+short if short else '')
            if i>=self.items.count():self.items.addItem(label)
            else:self.items.item(i).setText(label)
            item = self.items.item(i)
            background = QColor(r.background)
            item.setToolTip(f'遮盖背景色 {background.name().upper()}\n{r.text}'.rstrip())
            swatch = QPixmap(24, 24)
            swatch.setDevicePixelRatio(2)
            swatch.fill(Qt.GlobalColor.transparent)
            painter = QPainter(swatch)
            painter.setRenderHint(QPainter.RenderHint.Antialiasing)
            painter.setBrush(background)
            painter.setPen(QPen(QColor('#808791'), 1))
            painter.drawEllipse(QRectF(0.5, 0.5, 11, 11))
            painter.end()
            item.setIcon(QIcon(swatch))
        self.items.setCurrentRow(self.c.selection if index is None else index)
        self.loading = False
        self.choose(self.items.currentRow(),from_refresh=True)

    def selection_hint(self):
        if getattr(self.c,'waiting_for_target',False):
            if getattr(self.c, 'canvas_modes', False) and not getattr(self.c, 'application_id', ''):
                return '未选择应用。请从面板顶部的应用菜单选择或点选应用。'
            if getattr(self.c, 'application_id', ''):
                return f'等待 {self.c.application_name} 窗口。若有多个窗口，请从托盘选择。'
            return '等待 ChatGPT 窗口。可先编辑已保存的区域，打开或还原 ChatGPT 后自动附着。'
        if getattr(self.c,'calibration_preview',None):return '校准预览 · 尚未保存，确认前可取消'
        index = self.c.selection
        if not 0 <= index < len(self.c.regions):
            if self.c.regions:
                return '请在区域列表中选择一个区域。'
            return '先在目标窗口中拖动，创建并选中一个矩形。'
        region = self.c.regions[index]
        if region.position_mode == 'visual':
            status = getattr(self.c, 'tracking_states', {}).get(region.id, {}).get('state')
            message = {'FOLLOWING': '已定位', 'UNAVAILABLE': '组件或捕获不可用，可重试', 'NEEDS_MANUAL': '需要改回锚定、重新框选校准', 'PAUSED': '已暂停；完成编辑并切回目标后重新验证'}.get(status, '等待两次完整定位确认')
            if getattr(self.c, 'tracking_states', {}).get(region.id, {}).get('reason') == 'template_unavailable':
                message = '校准模板不可用'
            reason=getattr(self.c,'tracking_states',{}).get(region.id,{}).get('reason')
            if status=='HOLDING':
                return '视觉定位 · '+('检查中，继续显示上次确认位置' if reason=='checking_retained_position' else '保持上次确认位置；尺寸／DPI变化或定时检查后更新')+' · 位置与尺寸只读'
            if reason=='visual_cover_outside_client':
                message='遮盖按当前偏移位于窗口外；可调整偏移或放大窗口，结束编辑后会自动重新验证'
            elif reason in ('visual_result_stale','visual_sample_expired','visual_viewport_unavailable'):
                message='窗口或采样刚发生变化，正在自动重新定位'
            elif reason=='visual_result_outside_limits':message='本次位置超出允许范围，正在重新查找'
            elif reason=='visual_binding_changed':message='校准设置已变化，正在等待重新定位'
            if reason and reason.startswith('searching_'):
                message='正在分块找回目标，搜索范围 '+reason.removeprefix('searching_')+' DIP'
            elif reason=='search_ambiguous':message='找到多个相似目标，遮盖已隐藏；将稍后重新查找'
            elif reason=='search_not_found':message='暂未找回目标，遮盖已隐藏；将稍后重新查找'
            if reason=='compute_budget_exceeded':message='计算预算不足，请缩小参考区域或减少视觉区域'
            elif reason=='roi_occluded':message='参考区域被遮挡，已暂停'
            elif reason=='transient_occlusion':message='短暂遮挡，暂停采样并保留已确认位置'
            return '视觉定位 · '+message+' · 位置与尺寸只读'
        return '窗口锚定 · 随所选窗口边缘或中心定位'

    def choose(self, index,from_refresh=False):
        if self.loading:
            return
        regions=self.c.regions
        next_id=regions[index].id if 0<=index<len(regions) else None
        changed=next_id!=self._displayed_region
        if changed and not from_refresh and self.c.selection>=0:self.commit()
        self.loading = True
        self.c.selection = index
        self._displayed_region=next_id
        valid = 0 <= index < len(self.c.regions)
        self.pages.setEnabled(valid)
        self.region_title.setText((self.c.regions[index].text.splitlines() or ['空白遮盖'])[0][:28] if valid else '选择覆盖区域')
        self.set_feedback(self.selection_hint())
        r = self.c.regions[index] if 0 <= index < len(self.c.regions) else Region()
        preview=getattr(self.c,'calibration_preview',None)
        if preview and preview['id']!=r.id:preview=None
        if self.visual_controls:self.visual_controls.snapshot(r,preview,getattr(self.c,'visual_worker_available',False) and not getattr(self.c,'waiting_for_target',False))
        if preview:self.set_feedback('校准预览 · 尚未保存，确认后才替换原设置')
        for action, button in self.visual_buttons.items():
            button.setEnabled(valid and preview is None and (r.position_mode == 'visual' if action != 'calibrate' else
                r.position_mode == 'anchor' and getattr(self.c, 'visual_worker_available', False)) and
                (action=='anchor' or not getattr(self.c,'waiting_for_target',False)))
        focus=QApplication.focusWidget()
        text_active=focus is self.text or (focus and self.text.isAncestorOf(focus))
        if self.text.toPlainText() != r.text and (changed or not text_active):
            self.text.setPlainText(r.text)
        font_active=focus is self.font or (focus and self.font.isAncestorOf(focus))
        if self.font.currentFont().family()!=r.font and (changed or not font_active):self.font.setCurrentFont(QFont(r.font))
        self.bold.setChecked(r.bold)
        for key,button in self.color_buttons.items():
            value=getattr(r,key);swatch=QPixmap(16,16);swatch.fill(QColor(value))
            button.setText(value.upper());button.setIcon(QIcon(swatch))
        self.align.setCurrentIndex(["left", "center", "right"].index(r.align))
        for key, check in self.effect_checks.items():
            check.setChecked(getattr(r, key))
        self.direction.setCurrentIndex(["left", "right"].index(r.scroll_direction))
        for key, combo in self.anchor_controls.items():
            combo.setCurrentIndex(combo.findData(getattr(r, key)))
            combo.setEnabled(r.position_mode != 'visual' and preview is None)
            visible=r.position_mode!='visual' and preview is None
            combo.setVisible(visible);self.position_form.labelForField(combo).setVisible(visible)
        for key, widget in self.controls.items():
            sync_number(widget,getattr(r,key),force=changed)
            if key in ('x', 'y', 'width', 'height'):
                widget.setEnabled(r.position_mode != 'visual' and preview is None and not getattr(self.c,'waiting_for_target',False))
        self.loading = False
        self.c.repaint()

    def apply(self, *args):
        i = self.c.selection
        if self.loading or not 0 <= i < len(self.c.regions):
            return
        values = dict(text=self.text.toPlainText(), font=self.font.currentFont().family(), bold=self.bold.isChecked(),
                      align=["left", "center", "right"][self.align.currentIndex()])
        for key, check in self.effect_checks.items():
            values[key] = check.isChecked()
        values['scroll_direction'] = ["left", "right"][self.direction.currentIndex()]
        for key, combo in self.anchor_controls.items():
            values[key] = combo.currentData()
        for key, widget in self.controls.items():
            values[key] = widget.value()
        if self.c.regions[i].position_mode == 'visual' or getattr(self.c,'calibration_preview',None):
            for key in ('x', 'y', 'width', 'height', 'anchor_x', 'anchor_y'):
                values.pop(key, None)
        item=self.items.item(i)
        if item:item.setText(f"{i+1:02d}  {values['text'].replace(chr(10),' ')[:16] or '空白遮盖'}\n正在编辑")
        self.c.update_region(i, values)

    def commit(self):
        QApplication.inputMethod().commit()
        self.font.commit()
        if self.visual_controls:self.visual_controls.commit()
        for widget in self.controls.values():
            widget.interpretText()
        self.apply()

    def capture_state(self):
        return dict(page=self.pages.currentIndex(), geometry=self.saveGeometry())

    def restore_state(self, state):
        self.pages.setCurrentIndex(state.get('page', 0))
        self.sync_category(self.pages.currentIndex())
        if state.get('geometry'):
            self.restoreGeometry(state['geometry'])
            area=self.windowHandle().screen().availableGeometry()
            self.setMinimumSize(min(640,max(1,area.width()-32)),min(420,max(1,area.height()-32)))
            self.resize(min(self.width(),area.width()-32),min(self.height(),area.height()-32))
            self.move(max(area.left()+16,min(self.x(),area.right()-self.width()-15)),max(area.top()+16,min(self.y(),area.bottom()-self.height()-15)))

    def color(self, key):
        i = self.c.selection
        if i < 0:
            return
        color = QColorDialog.getColor(QColor(getattr(self.c.regions[i], key)), self, "选择颜色")
        if color.isValid():
            self.c.update_region(i, {key: color.name()})

    def delete_region_by_id(self, region_id):
        if not self.isVisible() or getattr(self.c, 'calibration_preview', None):
            return
        self.commit()
        for index, region in enumerate(self.c.regions):
            if region.id == region_id:
                self.c.delete_region(index)
                self.refresh()
                break

    def keyPressEvent(self, event):
        if event.key() == Qt.Key.Key_Escape:
            if getattr(self.c, 'canvas_mode', 'idle') != 'idle':
                self.c.set_canvas_mode('idle')
            else:
                self.c.set_editing(False)
        else:
            super().keyPressEvent(event)

    def closeEvent(self, event):
        event.ignore()
        self.c.set_editing(False)


class NativeFilter(QAbstractNativeEventFilter):
    def __init__(self, controller):
        super().__init__()
        self.controller = controller

    def nativeEventFilter(self, event_type, message):
        msg = native.W.MSG.from_address(int(message))
        if msg.message == 0x0312 and msg.wParam == 0x4347:
            self.controller.toggle()
            return True, 0
        return False, 0


class Controller(QObject):
    event_received = Signal(object, object)

    def __init__(self, app, store=None, demo=None, register_hotkey=True):
        super().__init__()
        self.app, self.store, self.demo = app, store or Store(), demo
        self.app.setFont(QFont("Microsoft YaHei UI", 9))
        self.target = int(demo.winId()) if demo else None
        self.editing = False
        self.panel_needs_placement = False
        self.edit_generation = 0
        self.closed = False
        self.dirty = False
        self.load_error = None
        try:
            self.regions = self.store.load()
        except (OSError, ValueError, TypeError) as exc:
            self.regions = []
            self.load_error = str(exc)
            # Preserve malformed files. Require an explicit reset before writes.
        self.canvas = Canvas(self.regions)
        self.surfaces = SurfaceManager(self, Canvas)
        self._inspector = None
        self._inspector_state = {}
        self.editor_session = EditSession(self)
        self.canvas.selected.connect(self.selection_changed)
        self.canvas.changed.connect(self.geometry_changed)
        self.canvas.edit_requested.connect(self.edit_text)
        self.canvas.done.connect(lambda: self.set_editing(False))
        self.canvas.drag_finished.connect(lambda: self.refresh_timer.start())
        self.save_timer = QTimer(self)
        self.save_timer.setSingleShot(True)
        self.save_timer.setInterval(350)
        self.save_timer.timeout.connect(self.save)
        self.refresh_timer = QTimer(self)
        self.refresh_timer.setSingleShot(True)
        self.refresh_timer.setInterval(16)
        self.refresh_timer.timeout.connect(self.sync)
        self.focus_timer = QTimer(self)
        self.focus_timer.setSingleShot(True)
        self.focus_timer.setInterval(120)
        self.focus_timer.timeout.connect(lambda: self.sync(foreground_grace=False))
        self.event_received.connect(self.on_event, Qt.ConnectionType.QueuedConnection)
        self.hooks = native.Hooks(self.win_event)
        self.filter = NativeFilter(self)
        app.installNativeEventFilter(self.filter)
        self.hotkey = bool(register_hotkey and native.u.RegisterHotKey(None, 0x4347, 0x4000 | 0x2 | 0x4, 0x45))
        self.tray = QSystemTrayIcon(self.make_icon(), self)
        menu = QMenu()
        self.edit_action = menu.addAction("编辑覆盖区域  Ctrl+Shift+E", self.toggle)
        menu.addAction("重新识别 ChatGPT 窗口", self.rediscover)
        menu.addAction("打开配置目录", self.open_config)
        self.startup = menu.addAction("开机自启")
        self.startup.setCheckable(True)
        self.startup.setChecked(self.startup_enabled())
        self.startup.triggered.connect(self.set_startup)
        menu.addAction("使用说明", self.help)
        menu.addSeparator()
        menu.addAction("退出", self.request_quit)
        self.menu = menu
        self.tray.setContextMenu(menu)
        self.tray.activated.connect(lambda reason: self.toggle() if reason == QSystemTrayIcon.ActivationReason.DoubleClick else None)
        self.tray.show()
        # Low-frequency watchdog handles missed events, restart and desktop switches.
        self.watchdog = QTimer(self)
        self.watchdog.setInterval(2000)
        self.watchdog.timeout.connect(self.sync)
        self.watchdog.start()
        app.aboutToQuit.connect(self.close)
        self.sync()
        if register_hotkey and not self.hotkey:
            self.notify("快捷键已被占用，请从托盘进入编辑。")
        if self.load_error:
            self.notify("配置读取失败，原文件已保留；请备份并移走配置后重启。" + self.load_error)

    def make_icon(self):
        pix = QPixmap(32, 32)
        pix.fill(QColor("#167d68"))
        p = QPainter(pix)
        p.setPen(QColor("white"))
        p.setFont(QFont("Segoe UI", 18, QFont.Weight.Bold))
        p.drawText(pix.rect(), Qt.AlignmentFlag.AlignCenter, "O")
        p.end()
        return QIcon(pix)

    @property
    def inspector(self):
        if self._inspector is None:
            self._inspector = Inspector(self.editor_session, self.canvas)
            self._inspector.restore_state(self._inspector_state)
        return self._inspector

    def selection_changed(self, index):
        if self._inspector is not None:
            self._inspector.refresh(index)

    def dispose_inspector(self):
        if self._inspector is not None:
            old, self._inspector = self._inspector, None
            self._inspector_state = old.capture_state()
            old.hide()
            old.deleteLater()

    def hide_inspector(self):
        if self._inspector is not None:
            self._inspector.hide()

    def notify(self, text):
        self.tray.showMessage("ChatGPT Overlay", text, QSystemTrayIcon.MessageIcon.Information, 5000)

    def win_event(self, hook, event, hwnd, obj, child, thread, time):
        # Filter before queuing: avoid flooding Qt with accessibility child events.
        if event == 3 or (hwnd == self.target and obj == 0 and child == 0) or (not self.target and event in (0x8000, 0x8002) and obj == 0):
            self.event_received.emit(event, hwnd)

    def on_event(self, event, hwnd):
        if self.closed:
            return
        if event == 3 and hwnd:
            # Reconcile queued events against the current foreground. NULL is a
            # normal activation transition, not evidence of an unrelated app.
            fg = native.u.GetForegroundWindow()
            if fg == hwnd and not self.demo and native.is_target(hwnd):
                self.select_target(hwnd, refresh=False)
            if fg == hwnd and not self.foreground_allowed(hwnd):
                self.hide_inspector()
        if event == 0x8001 and hwnd == self.target:
            self.canvas.hide()
            self.surfaces.hide()
            native.configure(int(self.canvas.winId()), None, self.editing)
            self.target = None
        if not self.refresh_timer.isActive():
            self.refresh_timer.start()

    def rediscover(self):
        if not self.demo:
            self.canvas.hide()
            self.surfaces.hide()
            self.target = None
        self.sync()

    def foreground_allowed(self, hwnd):
        return bool(hwnd) and (hwnd == self.target or (self.editing and native.pid(hwnd) == os.getpid()))

    def select_target(self, hwnd, refresh=True):
        """Attach to one deliberate visible ChatGPT window."""
        if not hwnd or not native.available(hwnd):
            return False
        changed = hwnd != self.target
        if changed:
            self.canvas.hide()
            self.surfaces.hide()
            native.configure(int(self.canvas.winId()), None, self.editing)
            self.target = hwnd
            self.panel_needs_placement = self.editing
        if refresh and self.editing and changed:
            self.inspector.refresh()
        if changed:
            self.sync()
        return True

    def sync(self, foreground_grace=True):
        if self.closed or self.canvas.picking:
            return
        foreground = native.u.GetForegroundWindow()
        if not self.demo and native.is_target(foreground):
            self.select_target(foreground, refresh=False)
        if not self.target or not native.u.IsWindow(self.target):
            self.target = int(self.demo.winId()) if self.demo else native.find_target()
        fg = native.u.GetForegroundWindow()
        target_available = native.available(self.target)
        if not fg and self.canvas.editing and target_available and self.canvas.isVisible() and foreground_grace:
            if not self.focus_timer.isActive():
                self.focus_timer.start()
            return
        self.focus_timer.stop()
        active = self.foreground_allowed(fg)
        visible = target_available
        self.tray.setToolTip("ChatGPT Overlay · " + ("编辑模式" if self.editing else "穿透模式") + (" · 已连接" if self.target else " · 等待 ChatGPT"))
        if not visible:
            self.canvas.hide()
            self.surfaces.hide()
            self.hide_inspector()
            return
        interactive = self.editing and active
        if self.canvas.editing != interactive:
            # Background overlays retain their pixels, but no edit chrome or
            # mouse interception. Resume editing when the target regains focus.
            self.canvas.set_editing(interactive, self.target)
        if not interactive:
            self.hide_inspector()
            self.canvas.start = self.canvas.draft = None
        if self.canvas.start is not None:
            # Preserve the coordinate frame while dragging. Visibility above
            # still reacts immediately if an unrelated app takes foreground.
            return
        box = native.client_box(self.target)
        if not box or box[2] <= 0 or box[3] <= 0:
            self.canvas.hide()
            self.surfaces.hide()
            return
        screen = screen_for_window(self.app, self.target)
        monitor = native.monitor_box(self.target)
        if monitor:
            geometry = canvas_geometry(screen, box, monitor[0])
            if not interactive:
                self.canvas.hide()
                if self.canvas.testAttribute(Qt.WidgetAttribute.WA_WState_Created):
                    self.canvas.destroy()
                    self.canvas.resize(1, 1)
                self.surfaces.sync(self.target, box, screen, geometry)
                return
            self.surfaces.hide()
            self.canvas.winId()
            if self.canvas.windowHandle().screen() is not screen:
                self.canvas.windowHandle().setScreen(screen)
            # Keep Qt's logical screen/geometry in sync with the native HWND.
            # A raw SetWindowPos alone leaves stale DPI/screen state in Qt.
            origin = QPointF()
            self.canvas.region_origin = origin
            if native.client_box(int(self.canvas.winId())) != box and self.canvas.geometry() != geometry:
                self.canvas.setGeometry(geometry)
        native.configure(int(self.canvas.winId()), self.target, interactive)
        self.canvas.show()
        # show() can alter native styles; restore click-through and owner afterwards.
        native.configure(int(self.canvas.winId()), self.target, interactive)
        # Stay immediately above the target even while another application is
        # active. Windows then occludes both at the same normal Z-order level.
        native.place(int(self.canvas.winId()), box, restack=not interactive or fg == self.target)
        if interactive:
            if self.panel_needs_placement:
                self.inspector.move_to_target()
                self.panel_needs_placement = False
            if not self.inspector.isVisible():
                self.inspector.show()
            # Qt maintains Canvas -> Inspector ownership; do not reparent or
            # reorder an active editor during every accessibility event.

    def toggle(self):
        self.set_editing(not self.editing)

    def set_editing(self, value):
        if not value and self.editing:
            if self._inspector is not None:
                self._inspector.commit()
            if not self.save():
                return False
        if value:
            foreground = native.u.GetForegroundWindow()
            if not self.demo and native.is_target(foreground):
                self.select_target(foreground, refresh=False)
            self.sync()
            if not self.target or not native.available(self.target):
                self.notify("请先打开 Windows 版 ChatGPT 客户端。")
                return
            native.u.SetForegroundWindow(self.target)
            if native.u.GetForegroundWindow() != self.target:
                self.notify("请先切换到 ChatGPT，再按 Ctrl+Shift+E。")
                return
        self.edit_generation += 1
        self.editing = value
        self.panel_needs_placement = value and not self._inspector_state
        self.canvas.set_editing(value, self.target)
        self.canvas.picking = False
        self.canvas.pick_callback = None
        self.canvas.setCursor(Qt.CursorShape.ArrowCursor)
        if value:
            self.inspector.refresh()
        else:
            self.dispose_inspector()
            if self.target and native.pid(native.u.GetForegroundWindow()) == os.getpid():
                native.u.SetForegroundWindow(self.target)
        self.sync()

    def geometry_changed(self):
        if self._inspector is not None:
            self._inspector.refresh()
        self.modified()

    def modified(self):
        self.canvas.update()
        for surface in self.surfaces.windows:
            surface.update()
        if not self.refresh_timer.isActive():
            self.refresh_timer.start()
        self.dirty = True
        self.save_timer.start()

    def save(self):
        if self.dirty and self.load_error:
            self.notify('配置未能读取，保留编辑会话以避免覆盖原文件。')
            return False
        if self.dirty and not self.load_error:
            try:
                self.store.save(self.regions)
                self.dirty = False
            except OSError as exc:
                self.notify("保存失败：" + str(exc))
                return False
        return True

    def edit_text(self, index):
        self.inspector.refresh(index)
        self.inspector.show()
        self.inspector.activateWindow()
        self.inspector.text.setFocus()
        self.inspector.text.selectAll()

    def pick_color(self, field='background'):
        if self.canvas.index < 0:
            self.notify("请先选中一个区域。")
            return
        index = self.canvas.index
        self.canvas.picking = True
        self.canvas.setCursor(Qt.CursorShape.CrossCursor)
        self.hide_inspector()
        generation = self.edit_generation
        self.canvas.pick_callback = lambda point: self.sample(point, index, generation, field)
        self.notify("点击目标窗口取色，按 Esc 取消。")

    def sample(self, point, index, generation=None, field='background'):
        if generation is not None and generation != self.edit_generation:
            return
        self.canvas.picking = False
        if not self.editing or not native.available(self.target):
            self.sync()
            return
        screen = self.app.screenAt(point)
        if screen and 0 <= index < len(self.regions):
            # QScreen coordinates and grabWindow sizes are in device-independent px.
            origin = screen.geometry().topLeft()
            image = screen.grabWindow(0, point.x()-origin.x(), point.y()-origin.y(), 1, 1).toImage()
            if not image.isNull():
                setattr(self.regions[index], field, image.pixelColor(0, 0).name())
                self.modified()
        self.sync()

    def startup_enabled(self):
        try:
            with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Run") as key:
                winreg.QueryValueEx(key, "ChatGPTOverlay")
            return True
        except OSError:
            return False

    def set_startup(self, enabled):
        try:
            with winreg.CreateKey(winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Run") as key:
                if enabled:
                    import subprocess
                    command = [sys.executable] if getattr(sys, "frozen", False) else [str(Path(sys.executable).with_name('pythonw.exe')), str(Path(__file__).resolve().parents[1] / 'main.py')]
                    winreg.SetValueEx(key, "ChatGPTOverlay", 0, winreg.REG_SZ, subprocess.list2cmdline(command))
                elif self.startup_enabled():
                    winreg.DeleteValue(key, "ChatGPTOverlay")
        except OSError as exc:
            self.startup.setChecked(self.startup_enabled())
            self.notify("设置开机自启失败：" + str(exc))

    def open_config(self):
        self.store.path.parent.mkdir(parents=True, exist_ok=True)
        os.startfile(self.store.path.parent)

    def help(self):
        QMessageBox.information(None, "ChatGPT Overlay", "Ctrl+Shift+E：切换编辑模式\n空白处拖动：新建区域\n拖动区域：移动；右下角：缩放\n双击：编辑文字\n方向键：微调；Shift：10 px\nDelete：删除；Esc：完成编辑\n\n非编辑模式鼠标穿透。仅目标窗口在前台时显示。\n配置：" + str(self.store.path))

    def request_quit(self):
        if self.editing:
            if self.set_editing(False) is False:
                return False
        elif not self.save():
            return False
        self.app.quit()
        return True

    def close(self):
        if self.closed:
            return
        if self._inspector is not None:
            self._inspector.commit()
        self.closed = True
        self.watchdog.stop()
        self.refresh_timer.stop()
        self.focus_timer.stop()
        self.save_timer.stop()
        self.save()
        self.hooks.close()
        if self.hotkey:
            native.u.UnregisterHotKey(None, 0x4347)
        self.app.removeNativeEventFilter(self.filter)
        self.canvas.hide()
        self.surfaces.close()
        self.dispose_inspector()
        self.tray.hide()
