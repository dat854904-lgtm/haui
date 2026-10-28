#!/usr/bin/env python3
"""End-to-end GTK4 activity log test on authorized VTXFS tmpfs image."""
import os
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import GLib
from vtxfs_files import Explorer, engine, STREAM_CHUNK

app = Explorer()
app.set_application_id("org.vtxfs.Files.LogSmoke")
dest = f"/log-smoke-{os.getpid()}.bin"
notices = []
app.notify = lambda title, message: notices.append((title, message))
completed = False

def activated(_):
    GLib.timeout_add(300, initial)

def initial():
    assert app.window is not None, "window not initialized"
    assert app.activity is not None, "activity log missing"
    assert app.activity.get_parent() is not None
    assert app.activity.get_visible()
    app.log_toggle.set_active(False)
    assert not app.activity.get_visible(), "log hide failed"
    app.log_toggle.set_active(True)
    assert app.activity.get_visible(), "log show failed"
    app.activity.post("INFO", "TEST-PANEL-STARTED")
    app._start_large_write(dest, 1048576)
    assert app.busy
    assert app.activity.get_sensitive(), "log must remain usable during a write"
    assert not app.files_page.get_sensitive(), "files page should be locked during write"
    GLib.timeout_add(100, poll)
    return GLib.SOURCE_REMOVE

def poll():
    if app.busy:
        return GLib.SOURCE_CONTINUE
    GLib.timeout_add(200, finish)
    return GLib.SOURCE_REMOVE

def finish():
    assert notices and notices[0][0] == "Đã ghi vào ổ ảo", notices
    assert engine("cat", dest) == STREAM_CHUNK * 16
    whole_log = "".join(app.activity.lines)
    for fragment in [
        "TEST-PANEL-STARTED", "put-stdin", "[CHẠY]",
        "[GHI]", "1 MiB", "[OK]", "Hoàn tất file",
        "ls /", "Hiển thị"
    ]:
        assert fragment in whole_log, f"{fragment!r} missing from activity log"
    assert "0x00" not in whole_log
    app.activity.copy()
    app.activity.clear()
    context = GLib.MainContext.default()
    while context.pending():
        context.iteration(False)
    assert "Đã xóa các dòng" in "".join(app.activity.lines)
    engine("remove", dest)
    print("PASS: GTK log pane, toggle, real command/start/progress/success, copy/clear, UI responsive", flush=True)
    app.window.close()
    app.quit()
    return GLib.SOURCE_REMOVE

app.connect("activate", activated)
raise SystemExit(app.run(sys.argv))
