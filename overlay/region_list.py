"""Region row actions and their pointer or keyboard anchors."""
from PySide6.QtCore import QEvent, QPoint, QPointF, QRect, Qt, Signal
from PySide6.QtGui import QPainter, QPalette
from PySide6.QtWidgets import QListWidget, QStyle, QStyledItemDelegate


class _RowActions(QStyledItemDelegate):
    def __init__(self, view):
        super().__init__(view)
        self.view = view

    @staticmethod
    def action_rect(row):
        return QRect(row.right() - 29, row.center().y() - 12, 24, 24)

    def paint(self, painter, option, index):
        super().paint(painter, option, index)
        hovered = option.state & QStyle.StateFlag.State_MouseOver
        focused = self.view.hasFocus() and index == self.view.currentIndex()
        if not (hovered or focused):
            return
        center = self.action_rect(option.rect).center()
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(Qt.PenStyle.NoPen)
        painter.setBrush(option.palette.color(QPalette.ColorRole.Text))
        for offset in (-5, 0, 5):
            painter.drawEllipse(QPointF(center.x() + offset, center.y()), 1.4, 1.4)
        painter.restore()

    def editorEvent(self, event, model, option, index):
        if (event.type() in (QEvent.Type.MouseButtonPress, QEvent.Type.MouseButtonRelease)
                and event.button() == Qt.MouseButton.LeftButton
                and self.action_rect(option.rect).contains(event.position().toPoint())):
            if event.type() == QEvent.Type.MouseButtonRelease:
                self.view.menu_requested.emit(index.row(), event.globalPosition().toPoint())
            return True
        return super().editorEvent(event, model, option, index)


class RegionList(QListWidget):
    menu_requested = Signal(int, QPoint)

    def __init__(self):
        super().__init__()
        self.setMouseTracking(True)
        self.setItemDelegate(_RowActions(self))
        self.setAccessibleName('覆盖区域列表')

    def contextMenuEvent(self, event):
        if event.reason() == event.Reason.Keyboard:
            self.open_current_menu()
        else:
            index = self.indexAt(event.pos())
            if index.isValid():
                self.menu_requested.emit(index.row(), event.globalPos())
        event.accept()

    def keyPressEvent(self, event):
        if (event.key() == Qt.Key.Key_Menu
                or (event.key() == Qt.Key.Key_F10 and event.modifiers() == Qt.KeyboardModifier.ShiftModifier)):
            self.open_current_menu()
            event.accept()
            return
        super().keyPressEvent(event)

    def open_current_menu(self):
        index = self.currentIndex()
        if index.isValid():
            anchor = self.itemDelegate().action_rect(self.visualRect(index)).bottomRight()
            self.menu_requested.emit(index.row(), self.viewport().mapToGlobal(anchor))
