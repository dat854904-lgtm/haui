#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""VTXFS Files — GNOME-style explorer for a VTXFS image in tmpfs (/dev/shm).

All navigation and mutations are executed by the real VTXFS userspace engine,
not by Python's host os filesystem APIs. No loop device, sudo, or physical
partition is involved. Data lasts until the RAM image is removed or reboot.
"""
import os
import re
import sys
import subprocess
import threading
import time
import io
import zipfile
from pathlib import Path
import gi

gi.require_version("Gtk", "4.0")
from gi.repository import Gtk, Gdk, GLib
from vtxfs_bitmap import BlockBitmap
from vtxfs_log import ActivityLog

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / "tools"
# Explicit alternative RAM volume sizes create separate images, never resize
# or format an existing volume. They have 128 MiB bootstrap overhead.
RAM_MIB = int(os.environ.get("VTXFS_RAM_MIB", "256"))
if RAM_MIB not in (256, 512, 1024):
    raise ValueError("VTXFS_RAM_MIB phải là 256, 512 hoặc 1024.")
IMAGE = Path("/dev/shm") / (
    f"vtxfs-files-{os.getuid()}.img" if RAM_MIB == 256
    else f"vtxfs-files-{os.getuid()}-{RAM_MIB}MiB.img")
MAX_TEXT = 4 * 1024 * 1024  # GUI editor only, not a filesystem size limit
STREAM_CHUNK = bytes(range(256)) * 256  # deterministic non-zero 64 KiB payload
ENGINE_LOGGER = None  # optional visible log panel; file data is never logged


def require_ram_volume():
    # Explicitly verify tmpfs, not merely an ordinary directory named shm.
    mounts = Path("/proc/self/mountinfo").read_text()
    if not any(line.split(" - ")[-1].startswith("tmpfs ") and
               line.split(" - ")[0].split()[4] == "/dev/shm"
               for line in mounts.splitlines()):
        raise RuntimeError("/dev/shm is not mounted as tmpfs. Refusing to create a disk image.")
    if IMAGE.is_symlink():
        raise RuntimeError("RAM image path must not be a symlink.")
    if not IMAGE.exists():
        result = subprocess.run([str(TOOLS / "mkfs.vtxfs"), "-L", "VTXFS-RAM",
                                 "-b", str(RAM_MIB * 256), str(IMAGE)], capture_output=True, text=True, timeout=60)
        if result.returncode:
            raise RuntimeError(result.stderr or result.stdout)
    if not IMAGE.is_file():
        raise RuntimeError("VTXFS RAM image must be a regular file.")


def engine(*args, input_data=None):
    # Log only command names, VTXFS paths, byte COUNTS and exit states.
    # File contents, binary data and host paths never go into the log.
    cmd = [str(TOOLS / "vtxfs-inspect"), str(IMAGE), *args]
    name = str(args[0]) if args else "info"
    target = str(args[1]) if len(args) > 1 else "/"
    summary = f"{name} {target}" + (
        f" · input {len(input_data):,} byte" if input_data is not None else "")
    start = time.monotonic()
    if ENGINE_LOGGER:
        ENGINE_LOGGER("CHẠY", summary)
    try:
        result = subprocess.run(cmd, input=input_data,
                                capture_output=True, timeout=240)
        elapsed = time.monotonic() - start
        if result.returncode:
            detail = (result.stderr.decode("utf-8", "replace").strip() or
                      result.stdout.decode("utf-8", "replace").strip() or
                      f"VTXFS returned error {result.returncode}")
            raise RuntimeError(detail)
        if ENGINE_LOGGER:
            count = len(result.stdout)
            ENGINE_LOGGER("OK",
                          f"{name} {target} · {elapsed:.2f}s · output {count:,} byte")
        return result.stdout
    except Exception as exc:
        if ENGINE_LOGGER:
            ENGINE_LOGGER("LỖI", f"{summary} · {type(exc).__name__}: {exc}")
        raise


def join_path(base, name):
    if not name or "/" in name or name in (".", "..") or "\0" in name:
        raise ValueError("Tên tệp không hợp lệ")
    if len(name.encode("utf-8")) > 255:
        raise ValueError("Tên tệp vượt quá 255 byte")
    return base.rstrip("/") + "/" + name


class Explorer(Gtk.Application):
    def __init__(self):
        super().__init__(application_id=(
            "org.vtxfs.Files" if RAM_MIB == 256
            else f"org.vtxfs.Files.Ram{RAM_MIB}"))
        self.cwd = "/"
        self.selected = None
        self.entries = []
        self.window = None
        self.busy = False
        self.activity = None
        self.file_buttons = []
        self.clipboard_path = None
        self.clipboard_cut = False
        self.search_term = ""
        self._menu_popover = None

    def do_activate(self):
        if self.window:
            self.window.present()
            return
        self.window = Gtk.ApplicationWindow(application=self)
        self.window.set_title("VTXFS Files — RAM Disk")
        self.window.set_default_size(1440, 770)
        key_events = Gtk.EventControllerKey.new()
        key_events.connect("key-pressed", self.on_keyboard)
        self.window.add_controller(key_events)
        provider = Gtk.CssProvider()
        provider.load_from_data(b"""
        .sidebar { background: alpha(@theme_fg_color, .035); padding: 15px; }
        .sidebar-title { font-weight: bold; font-size: 16px; }
        .location { font-weight: 700; font-size: 17px; }
        .file-row { padding: 11px 15px; }
        .pathbar { padding: 10px 16px; background: alpha(@theme_fg_color, .035); }
        .status { padding: 11px 16px; opacity: .78; }
        .hint { opacity: .65; }
        .vtxfs-log { border-left: 1px solid alpha(@theme_fg_color, .18);
                     background: alpha(@theme_fg_color, .025); }
        """)
        Gtk.StyleContext.add_provider_for_display(
            Gdk.Display.get_default(), provider, Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION)
        header = Gtk.HeaderBar()
        self.window.set_titlebar(header)
        self.btn_back = Gtk.Button(icon_name="go-previous-symbolic")
        self.btn_back.set_tooltip_text("Quay lại thư mục cha")
        self.btn_back.connect("clicked", lambda *_: self.go_up())
        header.pack_start(self.btn_back)
        self.file_buttons.append(self.btn_back)
        refresh_button = self.action_button("view-refresh-symbolic",
                                            "Làm mới", self.refresh)
        header.pack_start(refresh_button)
        self.file_buttons.append(refresh_button)
        self.log_toggle = Gtk.ToggleButton()
        self.log_toggle.set_icon_name("sidebar-show-right-symbolic")
        self.log_toggle.set_tooltip_text("Hiện/ẩn nhật ký hoạt động bên phải")
        self.log_toggle.set_active(True)
        header.pack_end(self.log_toggle)
        for icon, hint, callback in [
            ("folder-new-symbolic", "Thư mục mới", self.new_folder),
            ("document-new-symbolic", "Tệp văn bản mới", self.new_file),
            ("document-edit-symbolic", "Đổi tên", self.rename),
            ("user-trash-symbolic", "Xóa mục đã chọn", self.delete),
            ("emblem-ok-symbolic", "Kiểm tra fsck", self.check_fs),
            ("document-open-symbolic", "Nhập file từ máy (chỉ đọc file gốc)", self.import_file),
            ("drive-harddisk-symbolic", "Tạo dữ liệu thử 1–16 MiB trên ổ ảo", self.test_large_data),
        ]:
            action = self.action_button(icon, hint, callback)
            header.pack_end(action)
            self.file_buttons.append(action)

        paned = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        self.window.set_child(paned)
        sidebar = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=16)
        self.sidebar = sidebar
        sidebar.set_size_request(220, -1)
        sidebar.add_css_class("sidebar")
        title = Gtk.Label(label="VTXFS Files", xalign=0)
        title.add_css_class("sidebar-title")
        sidebar.append(title)
        info = Gtk.Label(
            label=f"Image RAM {RAM_MIB} MiB\n"
                  f"VAG dữ liệu tối đa {RAM_MIB - 128} MiB\n"
                  "Không ghi lên phân vùng thật",
                         xalign=0)
        info.add_css_class("hint")
        info.set_wrap(True)
        sidebar.append(info)
        root_btn = Gtk.Button(label="  Ổ VTXFS (RAM)")
        root_btn.connect("clicked", lambda *_: self.navigate("/"))
        sidebar.append(root_btn)
        trash_btn = Gtk.Button(label="  Thùng rác VTXFS")
        trash_btn.set_tooltip_text(
            "Duyệt các mục đã chuyển vào thùng rác bên trong image VTXFS")
        trash_btn.connect("clicked", self.open_trash)
        sidebar.append(trash_btn)
        help_label = Gtk.Label(label="Nhấp đúp để mở.\nChọn mục rồi đổi tên hoặc xóa.\nDữ liệu mất khi máy khởi động lại.", xalign=0)
        help_label.add_css_class("hint")
        help_label.set_wrap(True)
        sidebar.append(help_label)
        paned.set_start_child(sidebar)
        paned.set_position(236)

        content = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        paned.set_end_child(content)
        self.stack = Gtk.Stack()
        self.stack.set_vexpand(True)
        self.stack.set_hexpand(True)
        switcher = Gtk.StackSwitcher()
        switcher.set_stack(self.stack)
        switcher.set_halign(Gtk.Align.CENTER)
        content.append(switcher)
        content.append(self.stack)
        files_page = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        self.files_page = files_page
        files_and_log = Gtk.Paned(orientation=Gtk.Orientation.HORIZONTAL)
        files_and_log.set_shrink_start_child(False)
        files_and_log.set_shrink_end_child(False)
        files_and_log.set_position(600)
        files_page.set_size_request(350, -1)
        files_and_log.set_start_child(files_page)
        self.activity = ActivityLog()
        files_and_log.set_end_child(self.activity)
        self.log_toggle.connect("toggled", lambda b:
                                self.activity.set_visible(b.get_active()))
        self.stack.add_titled(files_and_log, "files", "Tệp và thư mục")
        self.bitmap = BlockBitmap(engine, self.error)
        self.stack.add_titled(self.bitmap, "bitmap", "Block Bitmap")
        self.stack.connect("notify::visible-child-name", self.on_tab_changed)

        path_bar = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
        path_bar.add_css_class("pathbar")
        self.path_label = Gtk.Label(label="/", xalign=0)
        self.path_label.add_css_class("location")
        path_bar.append(self.path_label)
        files_page.append(path_bar)

        actions = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=5)
        actions.set_margin_start(10)
        actions.set_margin_end(10)
        actions.set_margin_top(6)
        actions.set_margin_bottom(6)
        for label, method in [
            ("Sao chép", self.copy_selected),
            ("Cắt", self.cut_selected),
            ("Dán", self.paste),
            ("Nhân bản", self.duplicate),
            ("Chuyển tới…", self.move_to),
            ("Sao chép tới…", self.copy_to),
            ("Thuộc tính", self.properties),
            ("Khôi phục", self.restore_selected),
        ]:
            btn = Gtk.Button(label=label)
            btn.connect("clicked", method)
            btn.set_tooltip_text(label)
            actions.append(btn)
        self.search = Gtk.SearchEntry()
        self.search.set_placeholder_text("Tìm tên file…")
        self.search.set_hexpand(True)
        self.search.connect("search-changed", self.on_search_changed)
        actions.append(self.search)
        files_page.append(actions)

        scroller = Gtk.ScrolledWindow()
        scroller.set_vexpand(True)
        self.files_list = Gtk.ListBox()
        self.files_list.set_selection_mode(Gtk.SelectionMode.SINGLE)
        self.files_list.connect("row-selected", self.on_selected)
        self.files_list.connect("row-activated", self.on_activated)
        scroller.set_child(self.files_list)
        files_page.append(scroller)
        self.status = Gtk.Label(label="", xalign=0)
        self.status.add_css_class("status")
        files_page.append(self.status)
        self.window.present()
        global ENGINE_LOGGER
        ENGINE_LOGGER = self.activity.post
        self.log_event("INFO",
                       f"Mở VTXFS Files · RAM {RAM_MIB} MiB · volume {IMAGE.name}")
        try:
            require_ram_volume()
            self.log_event("OK", "Đã xác thực /dev/shm là tmpfs; image là file thường")
            self.refresh()
        except Exception as exc:
            self.error(str(exc))

    def on_tab_changed(self, _stack, _param):
        if self.stack.get_visible_child_name() == "bitmap":
            self.bitmap.open()

    def log_event(self, level, message):
        if self.activity:
            self.activity.post(level, message)

    def _set_busy(self, busy):
        self.busy = bool(busy)
        # Keep the log panel responsive while streaming data, but disallow
        # other filesystem mutations/read commands on the same image.
        if self.files_page:
            self.files_page.set_sensitive(not busy)
        if self.sidebar:
            self.sidebar.set_sensitive(not busy)
        for button in self.file_buttons:
            button.set_sensitive(not busy)
        self.stack.get_child_by_name("bitmap").set_sensitive(not busy)

    def action_button(self, icon, tooltip, callback):
        button = Gtk.Button(icon_name=icon)
        button.set_tooltip_text(tooltip)
        button.connect("clicked", callback)
        return button

    def error(self, message):
        self.log_event("LỖI", message)
        self.notify("Lỗi VTXFS", message)

    def notify(self, title, message):
        dialog = Gtk.MessageDialog(transient_for=self.window, modal=True,
                                   buttons=Gtk.ButtonsType.OK,
                                   message_type=Gtk.MessageType.INFO,
                                   text=title)
        dialog.format_secondary_text(str(message))
        dialog.connect("response", lambda d, *_: d.destroy())
        dialog.present()

    def ask(self, title, original, callback):
        dialog = Gtk.Dialog(title=title, transient_for=self.window, modal=True)
        dialog.add_button("Hủy", Gtk.ResponseType.CANCEL)
        dialog.add_button("Xác nhận", Gtk.ResponseType.OK)
        entry = Gtk.Entry()
        entry.set_text(original)
        entry.set_margin_top(18)
        entry.set_margin_bottom(18)
        entry.set_margin_start(18)
        entry.set_margin_end(18)
        dialog.get_content_area().append(entry)
        entry.set_activates_default(True)
        dialog.set_default_response(Gtk.ResponseType.OK)
        def finished(d, response):
            text = entry.get_text()
            d.destroy()
            if response == Gtk.ResponseType.OK:
                try:
                    result = callback(text)
                    if result is not False:
                        self.refresh()
                except Exception as exc:
                    self.error(str(exc))
        dialog.connect("response", finished)
        dialog.present()

    def refresh(self, *_):
        # Explicit refresh also reloads the current allocation bitmap snapshot.
        if self.stack.get_visible_child_name() == "bitmap":
            self.bitmap.reload()
        try:
            output = engine("ls", self.cwd).decode("utf-8", "replace")
            entries = []
            for line in output.splitlines():
                m = re.fullmatch(r"ino=(\d+) type=(\d+) name=(.*)", line)
                if m:
                    entries.append({"ino": int(m[1]), "type": int(m[2]), "name": m[3]})
            self.entries = sorted(entries, key=lambda e: (e["type"] != 2, e["name"].casefold()))
            row = self.files_list.get_first_child()
            while row:
                following = row.get_next_sibling()
                self.files_list.remove(row)
                row = following
            self.selected = None
            for item in self.entries:
                row = Gtk.ListBoxRow()
                row.item = item
                row.add_css_class("file-row")
                layout = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=16)
                icon_name = ("folder-symbolic" if item["type"] == 2 else
                             "text-x-generic-symbolic")
                icon = Gtk.Image.new_from_icon_name(icon_name)
                icon.set_pixel_size(27)
                layout.append(icon)
                name = Gtk.Label(label=item["name"], xalign=0)
                name.set_hexpand(True)
                layout.append(name)
                kind = Gtk.Label(label=("Thư mục" if item["type"] == 2 else "Tệp"))
                kind.add_css_class("hint")
                layout.append(kind)
                row.set_child(layout)
                rightclick = Gtk.GestureClick.new()
                rightclick.set_button(3)
                rightclick.connect(
                    "pressed", lambda gesture, n, x, y, item_row=row:
                    self.show_context_menu(item_row, x, y))
                row.add_controller(rightclick)
                self.files_list.append(row)
            self._apply_search()
            self.path_label.set_text("Ổ VTXFS  " + self.cwd)
            self.status.set_text(f"{len(self.entries)} mục · Ảnh VTXFS lưu trong RAM · {IMAGE}")
            self.btn_back.set_sensitive(self.cwd != "/" and not self.busy)
            self.log_event("INFO", f"Hiển thị {len(self.entries)} mục tại {self.cwd}")
        except Exception as exc:
            self.error(str(exc))

    def navigate(self, path):
        self.cwd = path
        if self.search:
            self.search.set_text("")
        self.stack.set_visible_child_name("files")
        self.refresh()

    def go_up(self):
        if self.cwd != "/":
            parent = self.cwd.rsplit("/", 1)[0] or "/"
            self.navigate(parent)

    def on_selected(self, _box, row):
        self.selected = getattr(row, "item", None) if row else None

    def on_activated(self, _box, row):
        item = getattr(row, "item", None)
        if not item:
            return
        path = join_path(self.cwd, item["name"])
        if item["type"] == 2:
            self.navigate(path)
        else:
            self.open_text(path)

    def current_item(self):
        if not self.selected:
            raise ValueError("Hãy chọn một tệp hoặc thư mục")
        return self.selected

    def on_search_changed(self, entry):
        self.search_term = entry.get_text().casefold().strip()
        self._apply_search()

    def _apply_search(self):
        visible = 0
        row = self.files_list.get_first_child()
        while row:
            item = getattr(row, "item", None)
            show = bool(item and self.search_term in item["name"].casefold())
            row.set_visible(show)
            visible += show
            row = row.get_next_sibling()
        if self.search_term:
            self.status.set_text(
                f"Tìm kiếm '{self.search_term}': {visible} / {len(self.entries)} mục")

    @staticmethod
    def _validate_path(path):
        if (not isinstance(path, str) or not path.startswith("/") or
                "\x00" in path or
                any(part in ("", ".", "..") for part in path.split("/")[1:])):
            if path != "/":
                raise ValueError("Đường dẫn phải tuyệt đối trong VTXFS và không có . hoặc ..")
        return path

    def _exists(self, path):
        self._validate_path(path)
        result = subprocess.run([str(TOOLS / "vtxfs-inspect"),
                                 str(IMAGE), "stat", path],
                                capture_output=True, timeout=30)
        if result.returncode == 0:
            return True
        if b"No such file or directory" in result.stderr:
            return False
        raise RuntimeError(result.stderr.decode("utf-8", "replace") or
                           "Không đọc được trạng thái đường dẫn VTXFS")

    def _unique_dest(self, base, original, suffix=" (bản sao)"):
        stem, dot, ext = original.rpartition(".")
        if not dot or not stem:
            stem, ext = original, ""
        else:
            ext = "." + ext
        for n in range(1, 500):
            marker = suffix if n == 1 else f"{suffix} {n}"
            name = stem + marker + ext
            if len(name.encode("utf-8")) > 255:
                raise ValueError("Tên file quá dài để nhân bản")
            dest = join_path(base, name)
            if not self._exists(dest):
                return dest
        raise RuntimeError("Không tìm được tên file bản sao khả dụng")

    def _selected_path(self):
        item = self.current_item()
        return join_path(self.cwd, item["name"]), item

    def copy_selected(self, *_):
        try:
            path, item = self._selected_path()
            if item["type"] != 1:
                raise ValueError("Sao chép hiện hỗ trợ file thường, chưa hỗ trợ sao chép cây thư mục.")
            self.clipboard_path, self.clipboard_cut = path, False
            self.log_event("INFO", f"Sao chép vào clipboard VTXFS: {path}")
            self.status.set_text(f"Đã sao chép {item['name']} · đi tới thư mục đích và Dán")
        except Exception as exc:
            self.error(str(exc))

    def cut_selected(self, *_):
        try:
            path, item = self._selected_path()
            self.clipboard_path, self.clipboard_cut = path, True
            self.log_event("INFO", f"Cắt trong VTXFS: {path}")
            self.status.set_text(f"Đã cắt {item['name']} · đi tới thư mục đích và Dán")
        except Exception as exc:
            self.error(str(exc))

    def _paste_to(self, target_dir):
        if not self.clipboard_path:
            raise ValueError("Chưa sao chép hoặc cắt file trong VTXFS")
        self._validate_path(target_dir)
        source = self.clipboard_path
        name = source.rsplit("/", 1)[-1]
        dest = join_path(target_dir, name)
        if source == dest:
            if self.clipboard_cut:
                self.log_event("INFO", "Nguồn và đích trùng nhau; không di chuyển")
                return
            dest = self._unique_dest(target_dir, name)
        if self._exists(dest):
            raise FileExistsError(f"Đích đã tồn tại: {dest}. Hãy đổi tên trước khi dán.")
        engine("move-safe" if self.clipboard_cut else "copy-file", source, dest)
        self.log_event("OK", f"{'Đã di chuyển' if self.clipboard_cut else 'Đã sao chép'}: {source} → {dest}")
        if self.clipboard_cut:
            self.clipboard_path = None
            self.clipboard_cut = False
        self.navigate(target_dir)

    def paste(self, *_):
        try:
            self._paste_to(self.cwd)
        except Exception as exc:
            self.error(str(exc))

    def duplicate(self, *_):
        try:
            path, item = self._selected_path()
            if item["type"] != 1:
                raise ValueError("Nhân bản hiện hỗ trợ file thường.")
            dest = self._unique_dest(self.cwd, item["name"])
            engine("copy-file", path, dest)
            self.log_event("OK", f"Nhân bản {path} → {dest}")
            self.refresh()
        except Exception as exc:
            self.error(str(exc))

    def _validate_directory(self, directory):
        directory = directory.strip()
        directory = directory.rstrip("/") or "/"
        directory = self._validate_path(directory)
        stat = engine("stat", directory).decode("utf-8", "replace")
        mode = re.search(r"mode\s*:\s*(\d+)", stat)
        if not mode or (int(mode.group(1), 8) & 0o170000) != 0o040000:
            raise ValueError("Đích phải là một thư mục VTXFS")
        return directory

    def _copy_into_dir(self, source, directory):
        directory = self._validate_directory(directory)
        name = source.rsplit("/", 1)[-1]
        dest = join_path(directory, name)
        if dest == source:
            dest = self._unique_dest(directory, name)
        engine("copy-file", source, dest)
        self.log_event("OK", f"Sao chép tới: {source} → {dest}")
        self.navigate(directory)
        return False

    def copy_to(self, *_):
        try:
            source, item = self._selected_path()
            if item["type"] != 1:
                raise ValueError("Sao chép cây thư mục chưa được hỗ trợ")
            self.ask("Sao chép tới thư mục VTXFS", "/",
                     lambda directory: self._copy_into_dir(source, directory))
        except Exception as exc:
            self.error(str(exc))

    def compress_zip(self, *_):
        try:
            source, item = self._selected_path()
            if item["type"] != 1:
                raise ValueError("Chỉ có thể tạo ZIP từ một file thường")
            stat = engine("stat", source).decode("utf-8", "replace")
            size = re.search(r"size_bytes\s*:\s*(\d+)", stat)
            if not size or int(size.group(1)) > 8 * 1024 * 1024:
                raise ValueError("Nén ZIP trên giao diện hỗ trợ file không quá 8 MiB")
            target = join_path(self.cwd, item["name"] + ".zip")
            if self._exists(target):
                raise FileExistsError("File ZIP đã tồn tại: " + target)
            self.log_event("CHẠY", f"Nén ZIP trong RAM: {source}")
            data = engine("cat", source)
            result = io.BytesIO()
            with zipfile.ZipFile(result, mode="w",
                                 compression=zipfile.ZIP_DEFLATED) as archive:
                archive.writestr(item["name"], data)
            engine("put-stdin", target, input_data=result.getvalue())
            self.log_event("OK", f"Đã tạo ZIP trong VTXFS: {target} "
                                     f"({len(data):,} → {result.tell():,} byte)")
            self.refresh()
        except Exception as exc:
            self.error(str(exc))

    def move_to(self, *_):
        try:
            source, _item = self._selected_path()
            def move(directory):
                directory = self._validate_path(directory.strip())
                stat = engine("stat", directory).decode("utf-8", "replace")
                mode = re.search(r"mode\s*:\s*(\d+)", stat)
                if not mode or (int(mode.group(1), 8) & 0o170000) != 0o040000:
                    raise ValueError("Đích phải là một thư mục VTXFS")
                dest = join_path(directory, source.rsplit("/", 1)[-1])
                if dest != source:
                    engine("move-safe", source, dest)
                self.log_event("OK", f"Đã di chuyển {source} → {dest}")
                self.navigate(directory)
                return False
            self.ask("Chuyển tới thư mục VTXFS (đường dẫn tuyệt đối)",
                     "/", move)
        except Exception as exc:
            self.error(str(exc))

    def properties(self, *_):
        try:
            path, item = self._selected_path()
            stat = engine("stat", path).decode("utf-8", "replace")
            text = (f"Vị trí: {path}\n"
                    f"Loại: {'Thư mục' if item['type'] == 2 else 'File'}\n"
                    f"Dung lượng image: {RAM_MIB} MiB\n\n{stat}"
                    "\nThông tin block được xem trong tab Block Bitmap.")
            self.notify("Thuộc tính VTXFS", text)
            self.log_event("INFO", f"Xem thuộc tính {path}")
        except Exception as exc:
            self.error(str(exc))

    def _perform_context_action(self, popup, func):
        popup.popdown()
        try:
            func()
        except Exception as exc:
            self.error(str(exc))

    def show_context_menu(self, row, x=12, y=12):
        if self.busy:
            return
        item = getattr(row, "item", None)
        if not item:
            return
        self.files_list.select_row(row)
        if self._menu_popover and self._menu_popover.get_parent():
            self._menu_popover.popdown()
            self._menu_popover.unparent()
        popup = Gtk.Popover()
        popup.set_has_arrow(True)
        popup.set_autohide(True)
        popup.set_parent(row)
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=3)
        box.set_margin_top(6)
        box.set_margin_bottom(6)
        box.set_margin_start(6)
        box.set_margin_end(6)
        path = join_path(self.cwd, item["name"])
        menu = [
            ("Mở thư mục" if item["type"] == 2 else "Mở bằng VTXFS Editor",
             lambda: self.navigate(path) if item["type"] == 2 else self.open_text(path)),
            ("Sao chép", self.copy_selected),
            ("Cắt", self.cut_selected),
            ("Dán vào thư mục này" if item["type"] == 2 else "Dán vào thư mục đang xem",
             lambda: self._paste_to(path if item["type"] == 2 else self.cwd)),
            ("Nhân bản", self.duplicate),
            ("Sao chép tới…", self.copy_to),
            ("Chuyển tới…", self.move_to),
            ("Nén thành ZIP (≤8 MiB)", self.compress_zip),
            ("Đổi tên…", self.rename),
            ("Thuộc tính", self.properties),
            ("Sao chép đường dẫn", self.copy_path),
            ("Đưa vào thùng rác", self.trash_selected),
            ("Khôi phục từ thùng rác", self.restore_selected),
            ("Xóa vĩnh viễn…", self.delete),
        ]
        for label, callback in menu:
            button = Gtk.Button(label=label)
            button.set_halign(Gtk.Align.FILL)
            if label in ("Sao chép", "Nhân bản", "Sao chép tới…",
                         "Nén thành ZIP (≤8 MiB)") and item["type"] == 2:
                button.set_sensitive(False)
            if label == "Khôi phục từ thùng rác" and self.cwd != "/.vtxfs-trash":
                button.set_sensitive(False)
            if label == "Đưa vào thùng rác" and self.cwd == "/.vtxfs-trash":
                button.set_sensitive(False)
            if label.startswith("Dán"):
                button.set_sensitive(bool(self.clipboard_path))
            button.connect(
                "clicked", lambda _btn, fn=callback:
                self._perform_context_action(popup, fn))
            box.append(button)
        popup.set_child(box)
        rect = Gdk.Rectangle()
        rect.x, rect.y, rect.width, rect.height = int(x), int(y), 2, 2
        popup.set_pointing_to(rect)
        self._menu_popover = popup
        popup.popup()
        self.log_event("INFO", f"Menu thao tác: {path}")

    def _ensure_trash(self):
        trash_dir = "/.vtxfs-trash"
        if not self._exists(trash_dir):
            engine("mkdir", "/", ".vtxfs-trash")
        return trash_dir

    def trash_selected(self, *_):
        try:
            source, item = self._selected_path()
            if source == "/.vtxfs-trash":
                raise ValueError("Không thể đưa thùng rác vào chính nó.")
            trash_dir = self._ensure_trash()
            if self.cwd == trash_dir:
                raise ValueError("File đã nằm trong thùng rác; dùng Xóa vĩnh viễn.")
            name = item["name"]
            stamp = str(int(time.time() * 1000))
            max_name = 255 - len(stamp) - 1
            # Byte-aware truncation preserves UTF-8 character boundaries.
            clipped = name.encode("utf-8")[:max_name].decode("utf-8", "ignore")
            dest = join_path(trash_dir, stamp + "-" + clipped)
            if self._exists(dest):
                dest = self._unique_dest(trash_dir, stamp + "-" + clipped)
            engine("move-safe", source, dest)
            self.log_event("INFO", f"Chuyển vào thùng rác VTXFS: {source} → {dest}")
            self.refresh()
        except Exception as exc:
            self.error(str(exc))

    def open_trash(self, *_):
        try:
            self.navigate(self._ensure_trash())
        except Exception as exc:
            self.error(str(exc))

    def copy_path(self, *_):
        try:
            source, _item = self._selected_path()
            display = Gdk.Display.get_default()
            if display:
                display.get_clipboard().set(source)
            self.log_event("INFO", f"Sao chép đường dẫn VTXFS: {source}")
        except Exception as exc:
            self.error(str(exc))

    def restore_selected(self, *_):
        try:
            if self.cwd != "/.vtxfs-trash":
                raise ValueError("Hãy vào 'Thùng rác VTXFS' để chọn file muốn khôi phục.")
            source, item = self._selected_path()
            name = item["name"]
            prefix, sep, original = name.partition("-")
            if not (sep and prefix.isdecimal() and original):
                raise ValueError("Không xác định được tên file gốc từ thùng rác")
            # Previous directory is not stored, so restore into the root.
            # Never overwrite a file already there.
            target = join_path("/", original)
            engine("move-safe", source, target)
            self.log_event("OK", f"Khôi phục từ thùng rác về /: {target}")
            self.navigate("/")
        except Exception as exc:
            self.error(str(exc))

    def on_keyboard(self, _controller, keyval, _keycode, state):
        if self.busy or self.stack.get_visible_child_name() != "files":
            return False
        if self.window.get_focus() == self.search and keyval != Gdk.KEY_Escape:
            return False
        ctrl = bool(state & Gdk.ModifierType.CONTROL_MASK)
        shift = bool(state & Gdk.ModifierType.SHIFT_MASK)
        if ctrl and keyval in (Gdk.KEY_c, Gdk.KEY_C):
            self.copy_selected()
        elif ctrl and keyval in (Gdk.KEY_x, Gdk.KEY_X):
            self.cut_selected()
        elif ctrl and keyval in (Gdk.KEY_v, Gdk.KEY_V):
            self.paste()
        elif ctrl and keyval in (Gdk.KEY_d, Gdk.KEY_D):
            self.duplicate()
        elif ctrl and keyval in (Gdk.KEY_f, Gdk.KEY_F):
            self.search.grab_focus()
        elif ctrl and keyval in (Gdk.KEY_r, Gdk.KEY_R):
            self.refresh()
        elif ctrl and shift and keyval in (Gdk.KEY_n, Gdk.KEY_N):
            self.new_folder()
        elif keyval == Gdk.KEY_F2:
            self.rename()
        elif keyval == Gdk.KEY_Delete:
            self.delete()
        elif keyval == Gdk.KEY_BackSpace:
            self.go_up()
        elif keyval == Gdk.KEY_Escape:
            self.search.set_text("")
        elif keyval == Gdk.KEY_Return and self.selected:
            path, item = self._selected_path()
            if item["type"] == 2:
                self.navigate(path)
            else:
                self.open_text(path)
        elif bool(state & Gdk.ModifierType.MOD1_MASK) and keyval == Gdk.KEY_Return:
            self.properties()
        else:
            return False
        return True

    def new_folder(self, *_):
        self.ask("Tạo thư mục", "Thư mục mới",
                 lambda text: engine("mkdir", self.cwd, text))

    def new_file(self, *_):
        def create(text):
            dest = join_path(self.cwd, text)
            if self._exists(dest):
                raise FileExistsError(f"File đã tồn tại: {dest}")
            engine("put-stdin", dest, input_data=b"")
        self.ask("Tạo tệp văn bản", "Tệp mới.txt", create)

    def rename(self, *_):
        try:
            item = self.current_item()
            old = join_path(self.cwd, item["name"])
            self.ask("Đổi tên", item["name"],
                     lambda new: engine("move-safe", old, join_path(self.cwd, new)))
        except Exception as exc:
            self.error(str(exc))

    def delete(self, *_):
        try:
            item = self.current_item()
            path = join_path(self.cwd, item["name"])
        except Exception as exc:
            self.error(str(exc))
            return
        dialog = Gtk.MessageDialog(transient_for=self.window, modal=True,
                                   buttons=Gtk.ButtonsType.OK_CANCEL,
                                   message_type=Gtk.MessageType.WARNING,
                                   text=f"Xóa '{item['name']}' khỏi đĩa RAM?")
        dialog.format_secondary_text("Thao tác này không thể hoàn tác.")
        def finished(d, response):
            d.destroy()
            if response == Gtk.ResponseType.OK:
                try:
                    engine("remove", path)
                    self.refresh()
                except Exception as exc:
                    self.error(str(exc))
        dialog.connect("response", finished)
        dialog.present()

    def open_text(self, path):
        try:
            stat = engine("stat", path).decode("utf-8")
            size = re.search(r"size_bytes\s*:\s*(\d+)", stat)
            if not size:
                raise RuntimeError("Không đọc được kích thước file")
            if int(size.group(1)) > MAX_TEXT:
                raise ValueError(
                    "File vượt quá 4 MiB, không mở trong trình sửa văn bản. "
                    "File vẫn được lưu đầy đủ trên ổ ảo VTXFS.")
            raw = engine("cat", path)
            if len(raw) > MAX_TEXT:
                raise ValueError("File vượt giới hạn trình sửa 4 MiB")
            content = raw.decode("utf-8")
            self.log_event("INFO", f"Đọc văn bản {path} · {len(raw):,} byte")
        except Exception as exc:
            self.error(str(exc))
            return
        dialog = Gtk.Window(transient_for=self.window, modal=True)
        dialog.set_title("VTXFS Editor — " + path)
        dialog.set_default_size(680, 510)
        column = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=8)
        column.set_margin_top(12)
        column.set_margin_bottom(12)
        column.set_margin_start(12)
        column.set_margin_end(12)
        dialog.set_child(column)
        scroller = Gtk.ScrolledWindow()
        scroller.set_vexpand(True)
        editor = Gtk.TextView()
        editor.set_monospace(True)
        editor.get_buffer().set_text(content)
        scroller.set_child(editor)
        column.append(scroller)
        buttons = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        buttons.set_halign(Gtk.Align.END)
        cancel = Gtk.Button(label="Hủy")
        cancel.connect("clicked", lambda *_: dialog.close())
        buttons.append(cancel)
        save = Gtk.Button(label="Lưu vào VTXFS")
        save.add_css_class("suggested-action")
        def write(*_):
            buf = editor.get_buffer()
            text = buf.get_text(buf.get_start_iter(), buf.get_end_iter(), True)
            try:
                encoded = text.encode("utf-8")
                if len(encoded) > MAX_TEXT:
                    raise ValueError(
                        "Trình sửa văn bản giới hạn 4 MiB; "
                        "bạn vẫn có thể nhập file lớn hơn vào ổ VTXFS.")
                self.log_event("GHI", f"Lưu văn bản {path} · {len(encoded):,} byte")
                engine("put-stdin", path, input_data=encoded)
                self.log_event("OK", f"Đã lưu văn bản {path}")
                dialog.close()
                self.refresh()
            except Exception as exc:
                self.error(str(exc))
        save.connect("clicked", write)
        buttons.append(save)
        column.append(buttons)
        dialog.present()

    def _check_destination_available(self, path):
        # The streaming CLI replaces existing files. Never overwrite without
        # asking the user: fail closed on anything except confirmed ENOENT.
        probe = subprocess.run(
            [str(TOOLS / "vtxfs-inspect"), str(IMAGE), "stat", path],
            capture_output=True, timeout=30)
        if probe.returncode == 0:
            raise FileExistsError("Tên file đã tồn tại trong VTXFS: " + path)
        if b"No such file or directory" not in probe.stderr:
            raise RuntimeError("Không xác minh được trạng thái file đích: " +
                               probe.stderr.decode("utf-8", "replace"))

    def _check_capacity(self, requested_bytes):
        info = engine("info").decode("utf-8")
        match = re.search(r"free_blocks\s*:\s*(\d+)", info)
        if not match:
            raise RuntimeError("Không thể xác định số block trống")
        free_blocks = int(match.group(1))
        # Space for metadata CoW and checkpoints; not an exact reservation.
        suggested_room = max(0, free_blocks - 1024) * 4096
        if requested_bytes > suggested_room:
            raise ValueError(
                f"Dữ liệu {requested_bytes / 1048576:.1f} MiB có thể vượt "
                f"dung lượng khả dụng (~{suggested_room / 1048576:.1f} MiB "
                "sau khi chừa chỗ cho metadata). "
                f"Image RAM {RAM_MIB} MiB có {RAM_MIB - 128} MiB vùng VAG.")

    def _stream_large(self, dest, source=None, count=None):
        """Runs on a worker thread; only writes inside the VTXFS image."""
        try:
            self.log_event("CHẠY",
                           f"put-stdin {dest} · chuyển dữ liệu theo đoạn 64 KiB")
            args = [str(TOOLS / "vtxfs-inspect"), str(IMAGE),
                    "put-stdin", dest]
            with subprocess.Popen(args, stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE) as proc:
                written = 0
                if source is None:
                    for _ in range(count):
                        proc.stdin.write(STREAM_CHUNK)
                        written += len(STREAM_CHUNK)
                        if written % (1024 * 1024) == 0:
                            self.log_event("GHI",
                                           f"{dest} · đã chuyển {written // 1048576:,} MiB tới stdin")
                            GLib.idle_add(self.status.set_text,
                                          f"Đang ghi {written // 1048576:,} MiB vào VTXFS…")
                else:
                    with open(source, "rb") as origin:
                        while True:
                            data = origin.read(64 * 1024)
                            if not data:
                                break
                            proc.stdin.write(data)
                            written += len(data)
                            if written % (1024 * 1024) == 0:
                                self.log_event("GHI",
                                               f"{dest} · đã chuyển {written // 1048576:,} MiB tới stdin")
                                GLib.idle_add(self.status.set_text,
                                              f"Đang nhập {written // 1048576:,} MiB vào VTXFS…")
                proc.stdin.close()
                output = proc.stdout.read()
                err = proc.stderr.read()
                code = proc.wait(timeout=90)
                if code:
                    raise RuntimeError(err.decode("utf-8", "replace") or
                                       output.decode("utf-8", "replace"))
                self.log_event("OK",
                               f"put-stdin {dest} · exit 0 · {written:,} byte · "
                               f"{output.decode('utf-8', 'replace').strip()}")
            GLib.idle_add(self._stream_finished, dest, written, None)
        except Exception as exc:
            GLib.idle_add(self._stream_finished, dest, 0, str(exc))

    def _stream_finished(self, dest, written, error):
        self._set_busy(False)
        if error:
            self.error("Ghi file lớn thất bại; file có thể đã ghi một phần. " + error)
        else:
            self.log_event("OK", f"Hoàn tất file {dest} · {written:,} byte")
            self.refresh()
            self.notify("Đã ghi vào ổ ảo",
                        f"{dest}\n{written:,} byte"
                        "\nKhông ghi dữ liệu vào phân vùng thật.")
        return GLib.SOURCE_REMOVE

    def _start_large_write(self, dest, total_bytes, source=None):
        require_ram_volume()
        self._check_destination_available(dest)
        self._check_capacity(total_bytes)
        self.log_event("INFO",
                       f"Bắt đầu ghi {dest} · tổng {total_bytes:,} byte")
        self._set_busy(True)
        self.status.set_text("Đang ghi vào VTXFS RAM…")
        count = total_bytes // len(STREAM_CHUNK) if source is None else None
        threading.Thread(target=self._stream_large,
                         args=(dest, source, count), daemon=True).start()
        return False

    def test_large_data(self, *_):
        def create(value):
            size = int(value)
            if size not in (1, 4, 16):
                raise ValueError("Chọn dung lượng 1, 4 hoặc 16 MiB.")
            filename = f"block-test-{size}MiB-{int(time.time())}.bin"
            return self._start_large_write(
                join_path(self.cwd, filename), size * 1048576)
        self.ask("Tạo file thử trong VTXFS: 1, 4 hoặc 16 MiB",
                 "4", create)

    def import_file(self, *_):
        # The native chooser only READS the explicitly selected host file;
        # VTXFS remains the only destination for bytes written.
        chooser = Gtk.FileChooserNative.new(
            "Chọn file cần nhập vào ổ ảo VTXFS", self.window,
            Gtk.FileChooserAction.OPEN, "Nhập", "Hủy")

        def response(dialog, choice):
            try:
                if choice != Gtk.ResponseType.ACCEPT:
                    return
                selected = dialog.get_file()
                source = selected.get_path() if selected else None
                if not source:
                    raise ValueError("Chỉ hỗ trợ tệp có đường dẫn cục bộ")
                src = Path(source)
                if not src.is_file() or src.is_symlink():
                    raise ValueError("Chỉ nhập tệp thường, không theo symlink")
                dest = join_path(self.cwd, src.name)
                self._start_large_write(dest, src.stat().st_size, src)
            except Exception as exc:
                self.error(str(exc))
            finally:
                dialog.destroy()
        chooser.connect("response", response)
        chooser.show()

    def check_fs(self, *_):
        try:
            self.log_event("CHẠY", f"fsck -q {IMAGE.name}")
            result = subprocess.run([str(TOOLS / "fsck.vtxfs"), "-q", str(IMAGE)],
                                    capture_output=True, text=True, timeout=90)
            if result.returncode:
                raise RuntimeError(result.stderr or result.stdout)
            self.log_event("OK", f"fsck {IMAGE.name} · không phát hiện lỗi")
            self.notify("FSCK", "Ảnh VTXFS trong RAM đã vượt qua kiểm tra.")
        except Exception as exc:
            self.error(str(exc))


if __name__ == "__main__":
    sys.exit(Explorer().run(sys.argv))
