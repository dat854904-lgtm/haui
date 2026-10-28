#!/usr/bin/env python3
"""Test the async GTK streaming action and optional source import on tmpfs."""
import hashlib
import os
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1] /"tools"))
import gi
gi.require_version("Gtk","4.0")
from gi.repository import GLib
from vtxfs_files import Explorer,engine,STREAM_CHUNK,IMAGE
app=Explorer()
app.set_application_id("org.vtxfs.Files.StreamSmoke")
dest=f"/__gui-big-{os.getpid()}.bin"
notice=[]
def activate(_):
    app.notify=lambda title,message: notice.append((title,message))
    GLib.timeout_add(200, begin)
def begin():
    app._start_large_write(dest,1048576)
    GLib.timeout_add(100,inspect)
    return GLib.SOURCE_REMOVE
def inspect():
    if app.busy:
        return GLib.SOURCE_CONTINUE
    try:
        assert notice, "async worker finished without notification"
        assert notice[0][0]=="Đã ghi vào ổ ảo",notice
        assert engine("cat",dest)==STREAM_CHUNK*16
        print("PASS: async GTK one-MiB generator and data roundtrip",flush=True)
    finally:
        engine("remove",dest)
        app.window.close()
        app.quit()
    return GLib.SOURCE_REMOVE
app.connect("activate",activate)
sys.exit(app.run(sys.argv))
