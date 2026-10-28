#!/usr/bin/env python3
"""GTK4 Files context menu and clipboard integration against RAM image."""
import os
import io
import zipfile
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import gi
gi.require_version("Gtk","4.0")
from gi.repository import GLib
from vtxfs_files import Explorer,engine

folder=f"/ui-actions-{os.getpid()}"
app=Explorer()
app.set_application_id("org.vtxfs.Files.ActionSmoke")
def on_notify(title,message):
    assert title != "Lỗi VTXFS", ("GUI reported error",message)
app.notify=on_notify
data=(b"Hello VTXFS context menu! " * 3500)
engine("mkdir","/",folder[1:])
engine("mkdir",folder,"dest")
engine("put-stdin",folder+"/original.txt",input_data=data)

def select(name):
    row=app.files_list.get_first_child()
    while row:
        item=getattr(row,"item",None)
        if item and item["name"]==name:
            app.files_list.select_row(row)
            assert app.selected is not None
            return row
        row=row.get_next_sibling()
    raise AssertionError(f"Cannot select {name}, displayed {app.entries}")

def process():
    app.navigate(folder)
    row=select("original.txt")
    app.show_context_menu(row,8,8)
    assert app._menu_popover and app._menu_popover.get_visible()
    app._menu_popover.popdown()
    app.search.set_text("orig")
    app.on_search_changed(app.search)
    visible=[r for r in (app.files_list.get_first_child(),)
             if r is not None]
    assert app.search_term=="orig"
    app.search.set_text("")
    select("original.txt")
    app.properties()
    app.compress_zip()
    archive = engine("cat",folder + "/original.txt.zip")
    with zipfile.ZipFile(io.BytesIO(archive)) as z:
        assert z.namelist() == ["original.txt"]
        assert z.read("original.txt") == data
    engine("remove",folder + "/original.txt.zip")
    select("original.txt")
    app._copy_into_dir(folder + "/original.txt", folder + "/dest")
    assert engine("cat",folder + "/dest/original.txt") == data
    engine("remove",folder + "/dest/original.txt")
    app.navigate(folder)
    select("original.txt")
    app.copy_selected()
    assert not app.clipboard_cut
    app.paste()
    assert engine("cat",folder+"/original (bản sao).txt")==data
    select("original (bản sao).txt")
    app.duplicate()
    assert engine("cat",folder+"/original (bản sao) (bản sao).txt")==data
    select("original (bản sao).txt")
    app.cut_selected()
    app.navigate(folder+"/dest")
    app.paste()
    assert engine("cat",folder+"/dest/original (bản sao).txt")==data
    assert "name=original (bản sao).txt" not in engine("ls",folder).decode()
    select("original (bản sao).txt")
    before = {row.split("name=", 1)[1] for row in engine("ls", "/.vtxfs-trash").decode().splitlines() if "name=" in row}
    app.trash_selected()
    assert "name=original (bản sao).txt" not in engine("ls",folder+"/dest").decode()
    after = {row.split("name=", 1)[1] for row in engine("ls", "/.vtxfs-trash").decode().splitlines() if "name=" in row}
    added = after - before
    assert len(added) == 1, added
    app.open_trash()
    select(added.pop())
    app.restore_selected()
    assert engine("cat", "/original (bản sao).txt") == data
    engine("remove", "/original (bản sao).txt")
    app.log_event("OK","Completed GUI contexts")
    print("PASS: GTK right-click popover, search, properties, copy, paste, duplicate, cut/move and Trash",flush=True)
    app.window.close()
    app.quit()
    return GLib.SOURCE_REMOVE

def activated(_):
    GLib.timeout_add(250,process)
app.connect("activate",activated)
try:
    code=app.run(sys.argv)
finally:
    # Clean up only this test's source and duplicated files; trash entry
    # belongs to test, identified by known unique source suffix.
    for path in (folder+"/original.txt",folder+"/original (bản sao) (bản sao).txt",
                 folder+"/dest/original (bản sao).txt"):
        try:engine("remove",path)
        except Exception:pass
    try:
        rows=engine("ls","/.vtxfs-trash").decode().splitlines()
        for line in rows:
            if "name=" in line and "original (bản sao).txt" in line:
                # unique test name prefix? trash lacks original folder path;
                # test can identify by inode and content, use only matching.
                path="/.vtxfs-trash/"+line.split("name=",1)[1]
                if engine("cat",path)==data:
                    engine("remove",path)
    except Exception:pass
    engine("remove",folder+"/dest")
    engine("remove",folder)
raise SystemExit(code)
