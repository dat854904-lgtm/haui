#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Visible, bounded, in-memory VTXFS operation log for GTK4.

This is an activity log of actual userspace commands and GUI progress,
not kernel printk/dmesg. It never logs file contents or writes to any disk.
Worker threads may call post(); GTK widgets are updated only by GLib.idle_add.
"""
from collections import deque
from datetime import datetime

from gi.repository import Gtk, Gdk, GLib


class ActivityLog(Gtk.Box):
    MAX_LINES = 750

    def __init__(self):
        super().__init__(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        self.set_size_request(285, -1)
        self.add_css_class("vtxfs-log")
        self.set_margin_top(9)
        self.set_margin_bottom(9)
        self.set_margin_start(12)
        self.set_margin_end(12)
        self.lines = deque(maxlen=self.MAX_LINES)
        self.follow = True

        heading = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=7)
        heading.append(Gtk.Image.new_from_icon_name("utilities-terminal-symbolic"))
        title = Gtk.Label(label="Nhật ký hoạt động", xalign=0)
        title.add_css_class("heading")
        title.set_hexpand(True)
        heading.append(title)

        clear_btn = Gtk.Button(icon_name="edit-clear-all-symbolic")
        clear_btn.set_tooltip_text("Xóa nhật ký đang hiển thị")
        clear_btn.connect("clicked", self.clear)
        heading.append(clear_btn)

        copy_btn = Gtk.Button(icon_name="edit-copy-symbolic")
        copy_btn.set_tooltip_text("Sao chép nhật ký (không bao gồm nội dung file)")
        copy_btn.connect("clicked", self.copy)
        heading.append(copy_btn)
        self.append(heading)

        hint = Gtk.Label(
            label="Lệnh thực thi • Kết quả • Tiến độ • Lỗi", xalign=0)
        hint.add_css_class("dim-label")
        self.append(hint)

        scroller = Gtk.ScrolledWindow()
        scroller.set_vexpand(True)
        scroller.set_hexpand(True)
        scroller.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        self.buffer = Gtk.TextBuffer()
        self.view = Gtk.TextView.new_with_buffer(self.buffer)
        self.view.set_editable(False)
        self.view.set_cursor_visible(False)
        self.view.set_monospace(True)
        self.view.set_wrap_mode(Gtk.WrapMode.WORD_CHAR)
        self.view.set_left_margin(6)
        self.view.set_right_margin(6)
        self.view.set_top_margin(8)
        self.view.set_bottom_margin(8)
        self.tags = {
            "OK": self.buffer.create_tag("vtxfs-ok", foreground="#29aa86"),
            "LỖI": self.buffer.create_tag("vtxfs-error", foreground="#e6665a"),
            "CHẠY": self.buffer.create_tag("vtxfs-run", foreground="#6f99e9"),
            "GHI": self.buffer.create_tag("vtxfs-write", foreground="#c58c30"),
        }
        scroller.set_child(self.view)
        self.append(scroller)

        self.count_label = Gtk.Label(label="0 sự kiện · chỉ trong phiên làm việc", xalign=0)
        self.count_label.add_css_class("dim-label")
        self.append(self.count_label)
        self.post("INFO", "Bảng nhật ký sẵn sàng. Không ghi log ra ổ vật lý.")

    @staticmethod
    def _sanitize(value):
        value = str(value).replace("\r", " ").replace("\n", " ↵ ")
        return "".join(ch if (ch.isprintable() or ch == "\t") else "?" for ch in value)[:500]

    def post(self, level, message):
        """Call from main or worker thread; never directly touch GTK from worker."""
        level = self._sanitize(level).upper()[:8]
        message = self._sanitize(message)
        GLib.idle_add(self._append, level, message)

    def _append(self, level, message):
        stamp = datetime.now().strftime("%H:%M:%S")
        line = f"[{stamp}] [{level}] {message}\n"
        full = len(self.lines) == self.MAX_LINES
        self.lines.append(line)
        if full:
            self.buffer.set_text("".join(self.lines))
        else:
            end = self.buffer.get_end_iter()
            tag = self.tags.get(level)
            if tag:
                self.buffer.insert_with_tags(end, line, tag)
            else:
                self.buffer.insert(end, line)
        self.count_label.set_text(
            f"{len(self.lines)} sự kiện gần nhất · chỉ trong phiên làm việc")
        if self.follow:
            self.view.scroll_to_iter(self.buffer.get_end_iter(), 0.0, False, 0, 0)
        return GLib.SOURCE_REMOVE

    def clear(self, *_):
        self.lines.clear()
        self.buffer.set_text("")
        self.count_label.set_text("0 sự kiện · chỉ trong phiên làm việc")
        self.post("INFO", "Đã xóa các dòng nhật ký trên giao diện.")

    def copy(self, *_):
        display = Gdk.Display.get_default()
        if display:
            display.get_clipboard().set("".join(self.lines))
            self.post("INFO", "Đã sao chép nhật ký vào clipboard.")
