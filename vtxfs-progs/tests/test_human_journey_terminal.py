#!/usr/bin/env python3
"""End-user VTXFS CLI diary: real subprocesses, independent tempfs image."""
import hashlib
import os
from pathlib import Path
import re
import subprocess
import sys
import time

ROOT=Path(__file__).resolve().parents[1]
IMG=Path("/dev/shm")/f"vtxfs-human-journey-{os.getpid()}.img"
CTL=ROOT/"tools/vtxfsctl"
MKFS=ROOT/"tools/mkfs.vtxfs"
REPORT=ROOT/"docs/VTXFS_HUMAN_CLI_JOURNEY_2026-10-26.md"
EVENTS=[]
CALLS=0
def tool(*args,data=None,expect=0):
    global CALLS
    CALLS+=1
    cp=subprocess.run([str(CTL),str(IMG),*map(str,args)],
                       input=data,capture_output=True,timeout=180)
    if expect=="fail":
        if cp.returncode==0:
            raise AssertionError(f"unexpected SUCCESS {args}: {cp.stdout[:160]!r}")
    elif cp.returncode!=expect:
        raise AssertionError(f"{args} rc={cp.returncode} stderr={cp.stderr[-650:]!r}")
    return cp.stdout
def exact(path,data):
    got=tool("cat",path)
    assert got==data, f"data mismatch {path}: {len(got)} vs {len(data)}"
def check(name,fn):
    try:
        details=fn() or "Đúng kỳ vọng."
        EVENTS.append(("PASS",name,details))
        print(f"PASS {len(EVENTS):02} {name}",flush=True)
    except Exception as exc:
        msg=f"{type(exc).__name__}: {str(exc)[:460]}"
        EVENTS.append(("FAIL",name,msg))
        print(f"FAIL {len(EVENTS):02} {name}: {msg}",flush=True)
def limit(name,details):
    EVENTS.append(("LIMIT",name,details))
    print(f"LIMIT {len(EVENTS):02} {name}: {details[:130]}",flush=True)
def baseline():
    assert b"Free alloc" in tool("disk-space")
    assert b"FSCK OK" in tool("fsck")
def start_project():
    for p in ("/Công việc","/Công việc/Dự án 2026",
              "/Công việc/Dự án 2026/Bản nháp","/Công việc/Dự án 2026/Bàn giao",
              "/Gia đình","/Gia đình/Ảnh","/Tải xuống"):
        tool("create",p,"--dir")
    assert "name=Dự án 2026".encode() in tool("ls","/Công việc")
def notes():
    text=("Kế hoạch tháng 10\nNgười phụ trách: nhóm A\n"
          "Ghi chú: thông tin tiếng Việt và emoji 🧪\n").encode()
    original="/Công việc/Dự án 2026/Bản nháp/Ghi chú họp.txt"
    tool("create",original)
    tool("edit",original,data=text)
    exact(original,text)
    newer=text+b"Da kiem tra lan 2.\n"
    tool("edit",original,data=newer)
    exact(original,newer)
    assert b"size_bytes" in tool("properties",original)
    return f"Tạo, ghi đè, đọc lại Unicode và nội dung nhiều dòng ({len(newer)} byte)."
def clean_edit():
    f="/Công việc/Dự án 2026/Bản nháp/draft.txt"
    tool("edit",f,data=b"A"*10000)
    tool("edit",f,data=b"short")
    exact(f,b"short")
    return "Ghi đè nội dung dài bằng nội dung ngắn, không để lại đuôi cũ."
def save_and_move():
    f="/Công việc/Dự án 2026/Bản nháp/Ghi chú họp.txt"
    tool("copy",f,"/Công việc/Dự án 2026/Bản nháp/Ghi chú dự phòng.txt")
    tool("rename",f,"Biên bản cuộc họp.txt")
    tool("cut","/Công việc/Dự án 2026/Bản nháp/Biên bản cuộc họp.txt")
    tool("paste","/Công việc/Dự án 2026/Bàn giao")
    path="/Công việc/Dự án 2026/Bàn giao/Biên bản cuộc họp.txt"
    assert b"Da kiem tra" in tool("cat",path)
    tool("copy-select",path)
    tool("paste","/Gia đình")
    exact("/Gia đình/Biên bản cuộc họp.txt",tool("cat",path))
    return "Copy dự phòng; đổi tên; cut-paste sang folder khác; copy-paste liên thư mục."
def hardlink_lifecycle():
    src="/Công việc/Dự án 2026/Bàn giao/Biên bản cuộc họp.txt"
    other="/Gia đình/Hợp đồng liên kết.txt"
    same="/Công việc/Dự án 2026/Bàn giao/Hồ sơ liên kết.txt"
    payload=tool("cat",src)
    tool("hardlink",src,other)
    tool("hardlink",src,same)
    exact(other,payload)
    exact(same,payload)
    assert b"nlink      : 3" in tool("stat",src)
    tool("readonly",src)
    assert b"0100444" in tool("stat",other)
    tool("edit",other,data=b"illegal",expect="fail")
    tool("readonly",src,"--off")
    tool("permanently-delete",same,"--yes")
    assert b"nlink      : 2" in tool("stat",other)
    exact(other,payload)
    tool("permanently-delete",other,"--yes")
    assert b"nlink      : 1" in tool("stat",src)
    exact(src,payload)
    return "Hard link cùng inode, nlink 3→2→1, readonly truyền qua mọi tên."

def search_sort():
    v=tool("search","biên bản","--in","/")
    assert "/Gia đình/Biên bản cuộc họp.txt".encode() in v
    assert "/Công việc/Dự án 2026/Bàn giao/Biên bản cuộc họp.txt".encode() in v
    for how in ("size","name","type","mtime"):
        assert tool("sort","/Gia đình","--by",how)
    assert b".txt" in tool("filter","/Gia đình","--ext",".txt")
    return "Tìm kiếm không phân biệt hoa thường, sắp xếp 4 cách, lọc theo phần mở rộng."
def binary():
    p="/Tải xuống/du-lieu-binary.dat"
    data=bytes(range(256))*8192+b"\0\0TRAILER"
    tool("edit",p,data=data)
    exact(p,data)
    tool("copy",p,"/Công việc/Dự án 2026/Bàn giao/du-lieu-binary-copy.dat")
    exact("/Công việc/Dự án 2026/Bàn giao/du-lieu-binary-copy.dat",data)
    return f"2 MiB dữ liệu nhị phân, NUL, SHA-256={hashlib.sha256(data).hexdigest()[:16]}…"
def symlink():
    orig="/Công việc/Dự án 2026/Bàn giao/Biên bản cuộc họp.txt"
    tool("symlink",orig,"/Biên bản mới nhất")
    assert tool("readlink","/Biên bản mới nhất").strip()==orig.encode()
    assert b"Link target:" in tool("properties","/Biên bản mới nhất")
    tool("copy","/Biên bản mới nhất","/Biên bản liên kết sao")
    assert tool("readlink","/Biên bản liên kết sao").strip()==orig.encode()
    return "Symlink và copy symlink giữ target đúng."
def trash_restore():
    f="/Gia đình/Biên bản cuộc họp.txt"
    original=tool("cat",f)
    tool("delete",f)
    lines=tool("ls","/.vtxfs-trash").decode().splitlines()
    assert len(lines)==1,lines
    entry=lines[0].split(" name=",1)[1]
    tool("restore",entry)
    exact(f,original)
    assert "name=Biên bản cuộc họp.txt".encode() in tool("ls","/Gia đình")
    return "Delete → Trash → Restore về đúng thư mục cũ, byte-exact."
def safe_collision():
    f="/Gia đình/Biên bản cuộc họp.txt"
    b=tool("cat",f)
    tool("copy",f,f,expect="fail")
    tool("rename",f,"../illegal.txt",expect="fail")
    tool("create",f,expect="fail")
    tool("copy","/Gia đình","/Gia đình/bên trong",expect="fail")
    exact(f,b)
    return "Thao tác sai không ghi đè file gốc."
def zip_extract():
    folder="/Công việc/Dự án 2026/Bàn giao"
    tool("zip",folder,"/Tải xuống/ban-giao.zip")
    tool("mkdir","/Tải xuống/Giải nén")
    tool("unzip","/Tải xuống/ban-giao.zip","/Tải xuống/Giải nén")
    content=tool("cat",folder+"/Biên bản cuộc họp.txt")
    exact("/Tải xuống/Giải nén/Bàn giao/Biên bản cuộc họp.txt",content)
    tool("unzip","/Tải xuống/ban-giao.zip","/Tải xuống/Giải nén",expect="fail")
    return "ZIP và giải nén nested; từ chối ghi đè lần thứ hai."
def hidden_readonly():
    f="/Công việc/Dự án 2026/Bản nháp/draft.txt"
    tool("hide",f)
    assert b"name=.draft.txt" in tool("ls","/Công việc/Dự án 2026/Bản nháp")
    tool("unhide","/Công việc/Dự án 2026/Bản nháp/.draft.txt")
    tool("readonly",f)
    assert b"0100444" in tool("stat",f)
    returned=tool("cat",f)
    # This CLI uses the portable engine, which does NOT enforce write permissions.
    cp=subprocess.run([str(CTL),str(IMG),"edit",f],input=b"unauthorized?",
                      capture_output=True,timeout=120)
    if cp.returncode==0:
        limit("READONLY KHÔNG được enforce",
              "File mode=0444 nhưng vtxfsctl edit vẫn thay đổi được nội dung. "
              "Đây là giới hạn portable-engine CLI, không phải kiểm thử quyền VFS kernel.")
        exact(f,b"unauthorized?")
    else:
        exact(f,returned)
    tool("readonly",f,"--off")
    assert b"0100644" in tool("stat",f)
    return "Ẩn/hiện tên dotfile và đổi mode 0444/0644 (xem giới hạn thực thi)."
def trash_collision():
    f="/Gia đình/Biên bản cuộc họp.txt"
    tool("delete",f)
    entry=tool("ls","/.vtxfs-trash").decode().split(" name=",1)[1].strip()
    tool("edit",f,data=b"new different document")
    tool("restore",entry,expect="fail")
    exact(f,b"new different document")
    tool("empty-trash",expect="fail")
    tool("empty-trash","--yes")
    assert tool("ls","/.vtxfs-trash").strip()==b""
    return "Restore không đè trùng tên; Empty Trash đòi --yes; dọn thùng rác."
def permanent():
    tool("permanently-delete","/Công việc/Dự án 2026/Bản nháp",expect="fail")
    tool("permanently-delete","/Công việc/Dự án 2026/Bản nháp","--yes")
    tool("stat","/Công việc/Dự án 2026/Bản nháp",expect="fail")
    tool("permanently-delete","/","--yes",expect="fail")
    return "Xóa đệ quy cần --yes, không cho xóa root."
def finish():
    assert b"FSCK OK" in tool("fsck")
    assert b"Total image" in tool("disk-space")
    return "fsck sạch sau chuỗi thao tác liên hoàn."
def main():
    assert not IMG.exists()
    mk=subprocess.run([str(MKFS),"-b","65536",str(IMG)],
                      capture_output=True,timeout=60)
    assert mk.returncode==0,mk.stderr
    tasks=[
        ("Khởi tạo volume, xem dung lượng, kiểm tra fsck",baseline),
        ("Tạo cấu trúc hồ sơ công việc/gia đình",start_project),
        ("Tạo/sửa/mở bản ghi Unicode",notes),
        ("Thay nội dung dài bằng ngắn",clean_edit),
        ("Sao lưu, rename, cut/copy/paste qua nhiều folder",save_and_move),
        ("Hard link, nlink và quyền xuyên nhiều tên",hardlink_lifecycle),
        ("Tìm kiếm, sort, filter",search_sort),
        ("File nhị phân 2 MiB và copy nguyên vẹn",binary),
        ("Symlink, shortcut và đọc thuộc tính",symlink),
        ("Delete/Trash/Restore nguyên vẹn",trash_restore),
        ("Tránh ghi đè trong lỗi/collision",safe_collision),
        ("Đóng gói và giải nén ZIP",zip_extract),
        ("Ẩn/hiện và kiểm tra readonly",hidden_readonly),
        ("Restore collision, Empty Trash",trash_collision),
        ("Permanent delete với --yes",permanent),
        ("FSCK cuối và dung lượng còn lại",finish),
    ]
    for name,fn in tasks:check(name,fn)
if __name__=="__main__":
    started=time.monotonic()
    try:main()
    finally:
        failures=sum(s=="FAIL" for s,_,_ in EVENTS)
        limits=sum(s=="LIMIT" for s,_,_ in EVENTS)
        lines=[
            "# Nhật ký trải nghiệm người dùng VTXFS — 26/10/2026",
            "",
            "Thiết bị: Arch Linux; tiện ích: tools/vtxfsctl (gọi subprocess thật).",
            "Mỗi tác vụ chạy trên một volume tmpfs mới trong /dev/shm; không chạm image thật.",
            "",
            f"**Kết quả: {len(EVENTS)-failures-limits} PASS, {limits} LIMIT, {failures} FAIL; "
            f"{CALLS} lệnh; {time.monotonic()-started:.1f} giây.**",
            "",
            "| Kết quả | Hành trình | Nhận xét |",
            "|---|---|---|"]
        for status,name,msg in EVENTS:
            lines.append(f"| {status} | {name.replace('|','/')} | "
                         f"{str(msg).replace('|','/').replace(chr(10),' ')[:400]} |")
        lines+=["","## Các giới hạn nhận biết",
                 "- Portable engine CLI không phải VFS permission boundary. Mode 0444 không bảo đảm chống ghi.",
                 "- Trash/clipboard được mô phỏng bằng metadata bên trong image; không phải Linux desktop trash.",
                 "- ZIP của CLI có giới hạn khoảng 8 MiB và không nén symlink.",
                 "- CLI chỉ nhận image thường nằm trực tiếp trong /dev/shm; không dùng làm mount helper.",
                 "- Báo cáo này không thay kiểm thử mounted POSIX trong QEMU, power-cut hay KASAN.",
                 "",f"Đã xóa image thử nghiệm: {IMG}"]
        REPORT.write_text("\n".join(lines)+"\n")
        if IMG.exists() and IMG.is_file() and not IMG.is_symlink():
            IMG.unlink()
        print(f"HUMAN_JOURNEY SUMMARY: PASS={len(EVENTS)-failures-limits} "
              f"LIMIT={limits} FAIL={failures} CALLS={CALLS} report={REPORT}",flush=True)
        if failures:sys.exit(1)
