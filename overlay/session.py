"""Small editor-facing API, independent of Canvas implementation details."""
from dataclasses import asdict, replace
from .model import Region


class EditSession:
    def __init__(self, controller):
        self.controller = controller

    @property
    def app(self):
        return self.controller.app

    @property
    def target(self):
        return self.controller.target

    @property
    def regions(self):
        return [replace(region) for region in self.controller.regions]

    @property
    def selection(self):
        return self.controller.canvas.index

    @selection.setter
    def selection(self, index):
        self.controller.canvas.index = index

    def update_region(self, index, values):
        if 0 <= index < len(self.controller.regions):
            original = self.controller.regions[index]
            values = {key: value for key, value in values.items() if key != 'id'}
            updated = Region.parse(dict(asdict(original), **values))
            original.__dict__.update(vars(updated))
            self.controller.modified()

    def delete_region(self, index):
        if 0 <= index < len(self.controller.regions):
            self.controller.regions.pop(index)
            self.selection = -1
            self.controller.modified()

    def repaint(self):
        self.controller.canvas.update()

    def select_target(self, hwnd):
        self.controller.select_target(hwnd)

    def pick_color(self, field='background'):
        self.controller.pick_color(field)

    def set_editing(self, value):
        self.controller.set_editing(value)
