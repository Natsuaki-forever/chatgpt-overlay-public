"""Small editor caption: custom close glyph, Qt-managed move and resize."""
from PySide6.QtCore import QEvent, QObject, QPointF, Qt
from PySide6.QtGui import QColor, QPainter, QPen
from PySide6.QtWidgets import QAbstractButton, QToolButton, QWidget


class CloseButton(QToolButton):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName('panelClose')
        self.setFixedSize(32, 32)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setAccessibleName('完成编辑并关闭面板')
        self.setToolTip('完成编辑并关闭面板 (Esc)')

    def paintEvent(self, event):
        super().paintEvent(event)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        color = self.palette().color(self.foregroundRole())
        if not self.isEnabled() or self.isDown():
            color.setAlpha(150)
        painter.setPen(QPen(color, 1.5))
        center = self.rect().center()
        x, y = center.x()+.5, center.y()+.5
        painter.drawLine(QPointF(x-5, y-5), QPointF(x+5, y+5))
        painter.drawLine(QPointF(x+5, y-5), QPointF(x-5, y+5))


class WindowChrome(QObject):
    def __init__(self, dialog, title_bar, close_button):
        super().__init__(dialog)
        self.dialog, self.title_bar, self.close_button = dialog, title_bar, close_button
        dialog.setMouseTracking(True)
        dialog.c.app.installEventFilter(self)

    def resize_edges(self, point):
        if self.dialog.isMaximized() or self.dialog.isFullScreen():
            return Qt.Edge(0)
        x, y = point.x(), point.y()
        width, height = self.dialog.width(), self.dialog.height()
        edges = Qt.Edge(0)
        if 0 <= x < 6:
            edges |= Qt.Edge.LeftEdge
        elif width-6 <= x < width:
            edges |= Qt.Edge.RightEdge
        if 0 <= y < 6:
            edges |= Qt.Edge.TopEdge
        elif height-6 <= y < height:
            edges |= Qt.Edge.BottomEdge
        return edges

    def eventFilter(self, watched, event):
        kind = event.type()
        if kind == QEvent.Type.Leave and watched is self.dialog:
            self.dialog.unsetCursor()
        if kind == QEvent.Type.Enter and isinstance(watched, QWidget) and watched.window() is self.dialog:
            self.dialog.unsetCursor()
        if kind not in (QEvent.Type.MouseMove, QEvent.Type.MouseButtonPress):
            return False
        if not isinstance(watched, QWidget) or watched.window() is not self.dialog or not self.dialog.isEnabled():
            return False
        point = self.dialog.mapFromGlobal(event.globalPosition().toPoint())
        edges = self.resize_edges(point)
        if kind == QEvent.Type.MouseMove:
            cursors = {
                Qt.Edge.LeftEdge: Qt.CursorShape.SizeHorCursor,
                Qt.Edge.RightEdge: Qt.CursorShape.SizeHorCursor,
                Qt.Edge.TopEdge: Qt.CursorShape.SizeVerCursor,
                Qt.Edge.BottomEdge: Qt.CursorShape.SizeVerCursor,
                Qt.Edge.LeftEdge | Qt.Edge.TopEdge: Qt.CursorShape.SizeFDiagCursor,
                Qt.Edge.RightEdge | Qt.Edge.BottomEdge: Qt.CursorShape.SizeFDiagCursor,
                Qt.Edge.RightEdge | Qt.Edge.TopEdge: Qt.CursorShape.SizeBDiagCursor,
                Qt.Edge.LeftEdge | Qt.Edge.BottomEdge: Qt.CursorShape.SizeBDiagCursor,
            }
            if edges:
                self.dialog.setCursor(cursors[edges])
            else:
                self.dialog.unsetCursor()
            return False
        if event.button() != Qt.MouseButton.LeftButton:
            return False
        window = self.dialog.windowHandle()
        if not window:
            return False
        if edges:
            return window.startSystemResize(edges)
        if watched is self.close_button or self.close_button.isAncestorOf(watched):
            return False
        control=watched
        while control and control is not self.title_bar:
            if isinstance(control,QAbstractButton):return False
            control=control.parentWidget()
        title_point = self.title_bar.mapFromGlobal(event.globalPosition().toPoint())
        if self.title_bar.rect().contains(title_point):
            return window.startSystemMove()
        return False
