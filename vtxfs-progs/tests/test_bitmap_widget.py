#!/usr/bin/env python3
"""GTK widget smoke test without external block devices."""
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/"tools"))
import gi
gi.require_version("Gtk", "4.0")
from gi.repository import Gtk
from vtxfs_files import engine
from vtxfs_bitmap import BlockBitmap
Gtk.init()
errors=[]
view=BlockBitmap(engine,errors.append)
view.open()
assert not errors, errors
assert view.loaded and view.snapshot
assert view.total_pages==32
assert "đã cấp phát" in view.summary.get_text()
view._goto(5)
assert view.page==5
view._on_grid_click(None,1,4,4)
assert view.selected_offset==5*1024
assert "PBLK" in view.inspector.get_text()
view.zoom_choice.set_selected(3)
assert view.cell==25
view.size_choice.set_selected(3)
assert view.page_size==2048 and view.page==2
view.reload()
assert not errors, errors
assert view.snapshot.allocated_count==view.snapshot.allocated
print("PASS: GTK4 Block Bitmap widget loads, pages, zooms, selects PBLK, refreshes")
