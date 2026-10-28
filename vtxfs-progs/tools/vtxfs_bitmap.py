#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Interactive GTK4 view of the authoritative VTXFS VAG allocation bitmap.

The read-only VTXFS CLI exports a complete VAG bitmap snapshot; the GUI
never parses the host device, never mounts a block device and never invents
the distinction between data and metadata ownership.
"""
import math
from dataclasses import dataclass

from gi.repository import Gtk


@dataclass(frozen=True)
class BitmapSnapshot:
    vag: int
    total_vags: int
    vag_blocks: int
    block_bytes: int
    start_pblk: int
    allocated: int
    free_blocks: int
    xid: int
    bits: bytes

    def allocated_at(self, off):
        if not 0 <= off < self.vag_blocks:
            raise IndexError(off)
        return bool(self.bits[off >> 3] & (1 << (off & 7)))

    @property
    def allocated_count(self):
        return sum(x.bit_count() for x in self.bits)


def parse_bitmap(output):
    """Fail closed if the engine's response is malformed or inconsistent."""
    fields = {}
    for line in output.decode("ascii", "strict").splitlines():
        key, sep, value = line.partition("=")
        if not sep or key in fields:
            raise ValueError("Dữ liệu bitmap từ VTXFS không hợp lệ")
        fields[key] = value
    if fields.get("format") != "VTXFS_BITMAP_V1":
        raise ValueError("Phiên bản dữ liệu bitmap không được hỗ trợ")
    values = {name: int(fields[name], 10) for name in (
        "vag", "total_vags", "vag_blocks", "block_bytes", "start_pblk",
        "start_offset", "count", "allocated", "free_blocks", "xid")}
    blocks = values["vag_blocks"]
    if (not 1 <= values["total_vags"] <= 64 or
            not 0 <= values["vag"] < values["total_vags"] or
            blocks != 32768 or values["block_bytes"] != 4096 or
            values["start_offset"] != 0 or values["count"] != blocks):
        raise ValueError("Hình học VAG/block không hợp lệ")
    bits = bytes.fromhex(fields["bitmap_hex"])
    if len(bits) != (blocks + 7) // 8:
        raise ValueError("Độ dài bitmap không đúng")
    snap = BitmapSnapshot(
        vag=values["vag"], total_vags=values["total_vags"],
        vag_blocks=blocks, block_bytes=values["block_bytes"],
        start_pblk=values["start_pblk"], allocated=values["allocated"],
        free_blocks=values["free_blocks"], xid=values["xid"], bits=bits)
    if (snap.allocated_count != snap.allocated or
            snap.allocated + snap.free_blocks != blocks):
        raise ValueError("Bitmap không khớp bộ đếm VAG")
    return snap


class BlockBitmap(Gtk.Box):
    PAGE_CHOICES = (256, 512, 1024, 2048)
    ZOOM_CHOICES = (12, 16, 20, 25)
    COLS = 32
    COLOR_USED = (0.11, 0.57, 0.48)
    COLOR_FREE = (0.81, 0.84, 0.87)
    COLOR_CHANGING = (0.95, 0.61, 0.15)
    COLOR_SELECT = (0.23, 0.45, 0.91)

    def __init__(self, engine, on_error):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=10)
        self.engine = engine
        self.on_error = on_error
        self.snapshot = None
        self.delta = bytes(4096)
        self.vag_index = 0
        self.page = 0
        self.selected_offset = None
        self.loaded = False
        self.set_margin_start(16)
        self.set_margin_end(16)
        self.set_margin_top(12)
        self.set_margin_bottom(10)

        title = Gtk.Label(label="Block Bitmap · VTXFS", xalign=0)
        title.add_css_class("title-2")
        self.append(title)
        subtitle = Gtk.Label(
            label="Trạng thái phân bổ từ bitmap VAG thật · chỉ đọc · không thay đổi block",
            xalign=0)
        subtitle.add_css_class("dim-label")
        self.append(subtitle)

        selectors = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        selectors.append(Gtk.Label(label="VAG"))
        self.vag_choice = Gtk.DropDown.new_from_strings(["VAG 0"])
        self.vag_choice.connect("notify::selected", self._vag_changed)
        selectors.append(self.vag_choice)

        selectors.append(Gtk.Label(label="Block/trang"))
        self.size_choice = Gtk.DropDown.new_from_strings(
            [str(x) for x in self.PAGE_CHOICES])
        self.size_choice.set_selected(2)
        self._last_page_size = self.page_size
        self.size_choice.connect("notify::selected", self._page_size_changed)
        selectors.append(self.size_choice)
        selectors.append(Gtk.Label(label="Zoom"))
        self.zoom_choice = Gtk.DropDown.new_from_strings(
            [f"{x}px" for x in self.ZOOM_CHOICES])
        self.zoom_choice.set_selected(2)
        self.zoom_choice.connect("notify::selected", self._zoom_changed)
        selectors.append(self.zoom_choice)

        refresh_button = Gtk.Button(label="↻ Cập nhật bitmap")
        refresh_button.connect("clicked", lambda *_: self.reload())
        selectors.append(refresh_button)
        self.append(selectors)

        self.summary = Gtk.Label(label="Chưa tải bitmap", xalign=0)
        self.append(self.summary)
        self.usage = Gtk.ProgressBar()
        self.usage.set_show_text(True)
        self.append(self.usage)

        legend = Gtk.Label(
            label="■ Xanh: đã cấp phát    ■ Xám: trống    "
                  "■ Cam: trạng thái đổi kể từ lần tải trước    "
                  "□ Viền xanh: block đang chọn", xalign=0)
        legend.add_css_class("dim-label")
        self.append(legend)

        overview_header = Gtk.Label(
            label="Tổng quan VAG · nhấp để chuyển tới vùng block", xalign=0)
        self.append(overview_header)
        self.overview = Gtk.DrawingArea()
        self.overview.set_content_width(780)
        self.overview.set_content_height(70)
        self.overview.set_hexpand(True)
        self.overview.set_draw_func(self._draw_overview)
        overview_click = Gtk.GestureClick()
        overview_click.connect("pressed", self._on_overview_click)
        self.overview.add_controller(overview_click)
        self.append(self.overview)

        navigation = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        self.prev_button = Gtk.Button(label="◀ Trước")
        self.prev_button.connect("clicked", lambda *_: self._goto(self.page - 1))
        self.next_button = Gtk.Button(label="Tiếp ▶")
        self.next_button.connect("clicked", lambda *_: self._goto(self.page + 1))
        self.page_spin = Gtk.SpinButton.new_with_range(1, 32, 1)
        self.page_spin.set_numeric(True)
        self.page_spin.connect("value-changed", self._page_spin_changed)
        self.page_label = Gtk.Label(label="Trang 1/32", xalign=0)
        navigation.append(self.prev_button)
        navigation.append(self.next_button)
        navigation.append(Gtk.Label(label="Đến trang"))
        navigation.append(self.page_spin)
        navigation.append(self.page_label)
        self.append(navigation)

        scroller = Gtk.ScrolledWindow()
        scroller.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        scroller.set_vexpand(True)
        self.grid = Gtk.DrawingArea()
        self.grid.set_draw_func(self._draw_blocks)
        self.grid.set_content_width(self.COLS * self.cell)
        self.grid.set_content_height(math.ceil(self.page_size / self.COLS) * self.cell)
        self.grid.set_halign(Gtk.Align.START)
        self.grid.add_controller(self._make_motion())
        click = Gtk.GestureClick()
        click.connect("pressed", self._on_grid_click)
        self.grid.add_controller(click)
        scroller.set_child(self.grid)
        self.append(scroller)

        self.inspector = Gtk.Label(
            label="Nhấp một ô để xem địa chỉ VBLK / PBLK / byte offset", xalign=0)
        self.inspector.set_selectable(True)
        self.inspector.set_wrap(True)
        self.append(self.inspector)
        note = Gtk.Label(
            label="Lưu ý: bitmap chỉ cho biết trống/đã cấp phát, "
                  "không biết block thuộc file nào. "
                  "Bootstrap PBLK trước vùng VAG không nằm trong biểu đồ.",
            xalign=0)
        note.add_css_class("dim-label")
        note.set_wrap(True)
        self.append(note)

    @property
    def page_size(self):
        return self.PAGE_CHOICES[self.size_choice.get_selected()]

    @property
    def cell(self):
        return self.ZOOM_CHOICES[self.zoom_choice.get_selected()]

    @property
    def total_pages(self):
        if not self.snapshot:
            return 1
        return math.ceil(self.snapshot.vag_blocks / self.page_size)

    def open(self):
        if not self.loaded:
            self.reload()

    def reload(self):
        try:
            old = self.snapshot
            fresh = parse_bitmap(self.engine("bitmap", str(self.vag_index)))
            if old and old.vag == fresh.vag:
                self.delta = bytes(x ^ y for x, y in zip(old.bits, fresh.bits))
            else:
                self.delta = bytes(len(fresh.bits))
            self.snapshot = fresh
            self.loaded = True
            if self.vag_choice.get_model().get_n_items() != fresh.total_vags:
                self.vag_choice.set_model(Gtk.StringList.new(
                    [f"VAG {i}" for i in range(fresh.total_vags)]))
                self.vag_choice.set_selected(self.vag_index)
            self.page = min(self.page, self.total_pages - 1)
            self._refresh_view()
        except Exception as exc:
            self.on_error(str(exc))

    def _vag_changed(self, choice, _param):
        new_index = choice.get_selected()
        if new_index >= 64 or new_index == self.vag_index:
            return
        self.vag_index = new_index
        self.page = 0
        self.selected_offset = None
        self.reload()

    def _page_size_changed(self, *_):
        first_offset = self.page * self._last_page_size
        self._last_page_size = self.page_size
        if self.snapshot:
            self.page = min(first_offset // self.page_size, self.total_pages - 1)
        self._refresh_view()

    def _zoom_changed(self, *_):
        self._refresh_view()

    def _page_spin_changed(self, spin):
        requested = int(spin.get_value()) - 1
        if self.snapshot and requested != self.page:
            self._goto(requested)

    def _goto(self, page):
        if not self.snapshot:
            return
        self.page = max(0, min(self.total_pages - 1, int(page)))
        self._refresh_view()

    def _refresh_view(self):
        if self.snapshot:
            s = self.snapshot
            percentage = s.allocated / s.vag_blocks * 100
            change_count = sum(b.bit_count() for b in self.delta)
            self.summary.set_text(
                f"VAG {s.vag} · {s.allocated:,} đã cấp phát / "
                f"{s.free_blocks:,} trống / {s.vag_blocks:,} block "
                f"· đã dùng {percentage:.2f}% · XID {s.xid}"
                f" · thay đổi từ lần tải trước: {change_count:,} block")
            self.usage.set_fraction(s.allocated / s.vag_blocks)
            self.usage.set_text(f"{percentage:.2f}% đã cấp phát")
        self.page_spin.set_range(1, self.total_pages)
        self.page_spin.set_value(self.page + 1)
        self.page_label.set_text(
            f"Trang {self.page + 1}/{self.total_pages} · "
            f"offset {self.page * self.page_size:,}–"
            f"{min((self.page + 1) * self.page_size, 32768)-1:,}")
        self.prev_button.set_sensitive(self.page > 0)
        self.next_button.set_sensitive(self.page < self.total_pages - 1)
        count = self.page_size
        self.grid.set_content_width(self.COLS * self.cell)
        self.grid.set_content_height(math.ceil(count / self.COLS) * self.cell)
        self.grid.queue_draw()
        self.overview.queue_draw()
        self._update_inspector()

    def _draw_blocks(self, _area, cr, _width, _height):
        if not self.snapshot:
            return
        start = self.page * self.page_size
        end = min(start + self.page_size, self.snapshot.vag_blocks)
        cell = self.cell
        for off in range(start, end):
            local = off - start
            x = (local % self.COLS) * cell
            y = (local // self.COLS) * cell
            cr.set_source_rgb(*(
                self.COLOR_USED if self.snapshot.allocated_at(off) else
                self.COLOR_FREE))
            cr.rectangle(x + 1, y + 1, cell - 2, cell - 2)
            cr.fill()
            if self.delta[off >> 3] & (1 << (off & 7)):
                cr.set_source_rgb(*self.COLOR_CHANGING)
                cr.set_line_width(2)
                cr.rectangle(x + 1.5, y + 1.5, cell - 3, cell - 3)
                cr.stroke()
            if self.selected_offset == off:
                cr.set_source_rgb(*self.COLOR_SELECT)
                cr.set_line_width(3)
                cr.rectangle(x + 1.5, y + 1.5, cell - 3, cell - 3)
                cr.stroke()

    def _draw_overview(self, _area, cr, width, height):
        if not self.snapshot:
            return
        blocks = self.snapshot.vag_blocks
        buckets = 256
        for i in range(buckets):
            start = i * blocks // buckets
            stop = (i + 1) * blocks // buckets
            used = sum(self.snapshot.allocated_at(n) for n in range(start, stop))
            ratio = used / (stop - start)
            # Gray => green as local occupancy goes from 0 to 100%.
            cr.set_source_rgb(*[
                self.COLOR_FREE[j] * (1 - ratio) + self.COLOR_USED[j] * ratio
                for j in range(3)])
            x = i * width / buckets
            next_x = (i + 1) * width / buckets
            cr.rectangle(x, 1, max(1, next_x - x + .1), height - 2)
            cr.fill()
        cr.set_source_rgb(*self.COLOR_SELECT)
        cr.set_line_width(2)
        x = (self.page * self.page_size / blocks) * width
        w = min(self.page_size, blocks - self.page * self.page_size) / blocks * width
        cr.rectangle(x + 1, 1.5, max(2, w - 2), height - 3)
        cr.stroke()

    def _on_overview_click(self, _gesture, _count, x, _y):
        if self.snapshot:
            width = max(1, self.overview.get_width())
            off = min(self.snapshot.vag_blocks - 1,
                      max(0, int(x / width * self.snapshot.vag_blocks)))
            self._goto(off // self.page_size)

    def _offset_at(self, x, y):
        if x < 0 or y < 0:
            return None
        col, row = int(x) // self.cell, int(y) // self.cell
        off = self.page * self.page_size + row * self.COLS + col
        if col >= self.COLS or off >= min(
                (self.page + 1) * self.page_size,
                self.snapshot.vag_blocks if self.snapshot else 0):
            return None
        return off

    def _on_grid_click(self, _gesture, _count, x, y):
        off = self._offset_at(x, y)
        if off is not None:
            self.selected_offset = off
            self._update_inspector()
            self.grid.queue_draw()

    def _make_motion(self):
        motion = Gtk.EventControllerMotion()
        motion.connect("motion", self._hover)
        return motion

    def _hover(self, _motion, x, y):
        off = self._offset_at(x, y)
        if off is None or not self.snapshot:
            self.grid.set_tooltip_text(None)
        else:
            pblk = self.snapshot.start_pblk + off
            state = "đã cấp phát" if self.snapshot.allocated_at(off) else "trống"
            self.grid.set_tooltip_text(
                f"VAG {self.snapshot.vag} · offset {off:,} · "
                f"VBLK {(self.snapshot.vag * 32768 + off):,} · "
                f"PBLK {pblk:,} · {state}")

    def _update_inspector(self):
        if not self.snapshot or self.selected_offset is None:
            self.inspector.set_text("Chọn một ô block để xem địa chỉ chi tiết.")
            return
        off = self.selected_offset
        s = self.snapshot
        pblk = s.start_pblk + off
        vblk = s.vag * s.vag_blocks + off
        state = "ĐÃ CẤP PHÁT" if s.allocated_at(off) else "TRỐNG"
        changed = bool(self.delta[off >> 3] & (1 << (off & 7)))
        self.inspector.set_text(
            f"Đã chọn: VAG {s.vag} · offset {off:,} · VBLK {vblk:,} "
            f"(0x{vblk:X}) · PBLK {pblk:,} (0x{pblk:X}) · "
            f"byte offset {pblk * s.block_bytes:,}–"
            f"{(pblk + 1) * s.block_bytes - 1:,} · {state}"
            f"{' · thay đổi từ lần tải trước' if changed else ''}")
