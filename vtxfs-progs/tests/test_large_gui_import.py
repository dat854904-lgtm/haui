#!/usr/bin/env python3
"""GTK test: optional import reads a tmpfs source, stores it in VTXFS only."""
import os,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import gi
gi.require_version("Gtk","4.0")
from gi.repository import GLib
from vtxfs_files import Explorer,engine,STREAM_CHUNK,require_ram_volume
require_ram_volume()
src=Path("/dev/shm")/f"vtxfs-import-source-{os.getpid()}.bin"
blob=STREAM_CHUNK*32  # 2 MiB
src.write_bytes(blob)
app=Explorer()
app.set_application_id("org.vtxfs.Files.ImportSmoke")
dst=f"/__gui-import-{os.getpid()}.bin"
notices=[]
def activate(_):
    app.notify=lambda title,message: notices.append((title,message))
    GLib.timeout_add(200,begin)
def begin():
    app._start_large_write(dst,len(blob),str(src))
    GLib.timeout_add(100,verify)
    return GLib.SOURCE_REMOVE
def verify():
    if app.busy:
        return GLib.SOURCE_CONTINUE
    try:
        assert notices and notices[0][0]=="Đã ghi vào ổ ảo",notices
        assert engine("cat",dst)==blob
        print("PASS: 2 MiB file import from tmpfs via GUI worker",flush=True)
    finally:
        engine("remove",dst)
        src.unlink()
        app.window.close()
        app.quit()
    return GLib.SOURCE_REMOVE
app.connect("activate",activate)
raise SystemExit(app.run(sys.argv))
