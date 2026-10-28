#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Terminal-first VTXFS file manager. All mutations are inside a RAM VTXFS image.

A host file is never copied/modified by these commands; only a regular VTXFS
image verified to be under /dev/shm on tmpfs is accepted. The only durable
clipboard and trash metadata live INSIDE the VTXFS image, not in host /tmp.
This tool is intentionally for developer/testing workflows, not a security
boundary. The portable engine does not enforce Unix ownership/permission.
"""
import argparse
import io
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import uuid
import zipfile

BASE = Path(__file__).resolve().parent
BINARY = BASE / "vtxfs-inspect"
TRASH = "/.vtxfs-trash"
META = "/.vtxfs-trash-meta"
CLIP = "/.vtxfs-cli-clipboard"
MAX_DEPTH = 64
MAX_ENTRIES = 10000
ZIP_LIMIT = 8 * 1024 * 1024


def ensure_ram_image(path):
    target = Path(path)
    mount = Path("/proc/self/mountinfo").read_text().splitlines()
    assert any(
        len(x.split(" - ")) == 2 and
        x.split(" - ")[0].split()[4] == "/dev/shm" and
        x.split(" - ")[1].split()[0] == "tmpfs"
        for x in mount
    ), "/dev/shm is not a tmpfs"
    # Fail closed on symlink, physical block devices, or paths outside tmpfs.
    if (target.is_symlink() or not target.is_file() or
            target.resolve().parent != Path("/dev/shm")):
        raise ValueError("Chỉ nhận image là file thường trực tiếp trong /dev/shm (tmpfs)")
    if target.stat().st_size < 256 * 1024 * 1024:
        raise ValueError("Image VTXFS nhỏ bất thường")
    return str(target.resolve())


def path_ok(path, allow_root=True):
    if not isinstance(path, str) or not path.startswith("/"):
        raise ValueError("Đường dẫn phải tuyệt đối bên trong VTXFS")
    if "\x00" in path or "\n" in path or "\r" in path:
        raise ValueError("Không hỗ trợ ký tự NUL/newline trong đường dẫn CLI")
    if path != "/" and (path.endswith("/") or "//" in path):
        raise ValueError("Không nhận đường dẫn có '/' thừa")
    if any(part in (".", "..") for part in path.split("/")[1:]):
        raise ValueError("Không cho phép . hoặc .. trong đường dẫn")
    if len(path.encode("utf-8")) > 4096:
        raise ValueError("Đường dẫn quá dài")
    if any(len(x.encode("utf-8")) > 255 for x in path.split("/")[1:]):
        raise ValueError("Thành phần tên quá 255 byte")
    if not allow_root and path == "/":
        raise ValueError("Không được thao tác phá hủy thư mục gốc")
    return path


def parent_name(path):
    path_ok(path, allow_root=False)
    head, _, name = path.rpartition("/")
    return head or "/", name


def join(parent, name):
    if not name or name in (".", "..") or "/" in name:
        raise ValueError("Tên không hợp lệ")
    return path_ok((parent.rstrip("/") or "") + "/" + name)


class Volume:
    def __init__(self, image):
        self.image = ensure_ram_image(image)

    def call(self, *arguments, data=None, output=False):
        cmd = [str(BINARY), self.image, *map(str, arguments)]
        proc = subprocess.run(cmd, input=data, capture_output=True, timeout=300)
        if proc.returncode:
            err = proc.stderr.decode("utf-8", "replace").strip()
            raise RuntimeError(f"{arguments[0]}: {err or 'exit=' + str(proc.returncode)}")
        if output:
            sys.stdout.buffer.write(proc.stdout)
        return proc.stdout

    def kind(self, path):
        out = self.call("stat", path_ok(path)).decode("utf-8", "replace")
        m = re.search(r"^\s*mode\s*:\s*0?([0-7]+)\s*$", out, flags=re.M)
        if not m:
            raise ValueError("Không đọc được mode từ stat")
        return stat.S_IFMT(int(m.group(1), 8))

    def exists(self, path):
        # Do not swallow corruption/I/O errors as "file not found".
        result = subprocess.run([str(BINARY), self.image, "stat", path_ok(path)],
                                capture_output=True, timeout=30)
        if not result.returncode:
            return True
        if b"No such file or directory" in result.stderr:
            return False
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))

    def entries(self, directory):
        if self.kind(directory) != stat.S_IFDIR:
            raise NotADirectoryError(directory)
        lines = self.call("ls", directory).decode("utf-8", "strict").splitlines()
        entries = []
        for line in lines:
            m = re.fullmatch(r"ino=(\d+) type=(\d+) name=(.*)", line)
            if not m or not m.group(3):
                raise ValueError(f"Malformed directory entry: {line!r}")
            entries.append((m.group(3), int(m.group(2))))
        if len(entries) > MAX_ENTRIES:
            raise ValueError("Thư mục chứa quá nhiều mục để xử lý an toàn")
        return entries

    def mkdir(self, path):
        parent, name = parent_name(path)
        self.call("mkdir", parent, name)

    def assert_writable(self, path):
        """CLI write policy: enforce mode write bits for existing regular files.

        This protects commands in this CLI only; direct portable-engine
        calls are not Unix identity/ACL enforcement and can still bypass it.
        """
        path_ok(path, False)
        if not self.exists(path):
            return
        if self.kind(path) != stat.S_IFREG:
            raise PermissionError("Không thể ghi đè đối tượng không phải file thường")
        raw = self.call("stat", path).decode("utf-8", "replace")
        m = re.search(r"^\s*mode\s*:\s*0?([0-7]+)\s*$", raw, re.M)
        if not m or not (int(m.group(1), 8) & 0o222):
            raise PermissionError("File ở chế độ readonly (không có bit ghi)")

    def put(self, path, data):
        self.assert_writable(path)
        self.call("put-stdin", path_ok(path, False), data=data)

    def read(self, path):
        return self.call("cat", path_ok(path))

    def create(self, path, is_dir=False):
        if self.exists(path):
            raise FileExistsError(path)
        if is_dir:
            self.mkdir(path)
        else:
            self.put(path, b"")

    def copy_tree(self, source, target, depth=0):
        path_ok(source, False)
        path_ok(target, False)
        if depth > MAX_DEPTH:
            raise ValueError("Thư mục quá sâu")
        kind = self.kind(source)
        if kind == stat.S_IFDIR and target.startswith(source + "/"):
            raise ValueError("Không sao chép thư mục vào bên trong chính nó")
        if self.exists(target):
            raise FileExistsError(target)
        if kind == stat.S_IFDIR:
            self.mkdir(target)
            for name, _type in self.entries(source):
                self.copy_tree(join(source, name), join(target, name), depth + 1)
        elif kind == stat.S_IFREG:
            self.call("copy-file", source, target)
        elif kind == stat.S_IFLNK:
            link = self.call("readlink", source).decode("utf-8", "strict").rstrip("\n")
            self.call("symlink", link, target)
        else:
            raise ValueError("Không hỗ trợ sao chép kiểu inode này")

    def remove_tree(self, path, depth=0):
        path_ok(path, False)
        if depth > MAX_DEPTH:
            raise ValueError("Thư mục quá sâu")
        if self.kind(path) == stat.S_IFDIR:
            for name, _type in self.entries(path):
                self.remove_tree(join(path, name), depth + 1)
        self.call("remove", path)

    def ensure_folder(self, path):
        if not self.exists(path):
            self.mkdir(path)
        if self.kind(path) != stat.S_IFDIR:
            raise ValueError(f"Không phải thư mục: {path}")

    def json_read(self, path):
        return json.loads(self.read(path).decode("utf-8"))

    def json_put(self, path, value):
        data = json.dumps(value, ensure_ascii=False,
                          separators=(",", ":")).encode("utf-8")
        if len(data) > 4000:
            raise ValueError("Metadata vượt kích thước cho phép")
        self.put(path, data)

    def trash(self, source):
        path_ok(source, False)
        if (source in (TRASH, META, CLIP) or
                source.startswith(TRASH + "/") or
                source.startswith(META + "/")):
            raise ValueError("Không thể đưa vùng quản lý nội bộ vào thùng rác")
        self.kind(source)
        self.ensure_folder(TRASH)
        self.ensure_folder(META)
        ticket = uuid.uuid4().hex
        name = source.rsplit("/", 1)[1]
        # 32 hex + '-' + <=222 name bytes fits VTXFS max name length.
        shortened = name.encode("utf-8")[:210].decode("utf-8", "ignore")
        dest = join(TRASH, ticket + "-" + shortened)
        metapath = join(META, ticket + ".json")
        self.json_put(metapath, {"source": source, "ticket": ticket})
        try:
            self.call("move-safe", source, dest)
        except Exception:
            self.call("remove", metapath)
            raise
        print(f"TRASHED {source} -> {dest}")

    def restore(self, entry):
        trashpath = (entry if entry.startswith("/")
                     else join(TRASH, entry))
        path_ok(trashpath, False)
        if not trashpath.startswith(TRASH + "/"):
            raise ValueError("Chỉ khôi phục từ thùng rác VTXFS")
        ticket = trashpath.rsplit("/", 1)[1].split("-", 1)[0]
        if not re.fullmatch("[0-9a-f]{32}", ticket):
            raise ValueError("Không có metadata khôi phục cho mục này")
        metapath = join(META, ticket + ".json")
        meta = self.json_read(metapath)
        original = path_ok(meta["source"], False)
        if original.startswith(TRASH + "/") or original.startswith(META + "/") or original in (TRASH, META):
            raise ValueError("Đích khôi phục không hợp lệ (nằm trong thư mục hệ thống)")
        if meta["ticket"] != ticket:
            raise ValueError("Metadata thùng rác bị lệch")
        parent, _name = parent_name(original)
        if self.kind(parent) != stat.S_IFDIR:
            raise ValueError("Thư mục gốc không còn tồn tại")
        if self.exists(original):
            raise FileExistsError(f"Đích khôi phục đã tồn tại: {original}")
        self.call("move-safe", trashpath, original)
        self.call("remove", metapath)
        print(f"RESTORED {original}")

    def empty_trash(self):
        self.ensure_folder(TRASH)
        for name, _type in list(self.entries(TRASH)):
            self.remove_tree(join(TRASH, name))
        if self.exists(META):
            for name, _type in list(self.entries(META)):
                self.remove_tree(join(META, name))
        print("EMPTY TRASH: all discarded VTXFS directory entries removed")

    def list_sorted(self, directory, by="name", ext=None, hidden=True):
        entries = self.entries(directory)
        if ext:
            entries = [(n,t) for n,t in entries if n.casefold().endswith(ext.casefold())]
        if not hidden:
            entries = [(n,t) for n,t in entries if not n.startswith(".")]
        def score(item):
            name,typ = item
            if by == "name": return name.casefold()
            if by == "type": return (typ,name.casefold())
            path = join(directory,name)
            properties = self.call("stat", path).decode()
            field = "size_bytes" if by=="size" else "mtime_ns"
            m = re.search(r"^\s*"+field+r"\s*:\s*(\d+)", properties, re.M)
            if not m: raise RuntimeError("stat missing " + field)
            return (int(m.group(1)),name.casefold())
        entries.sort(key=score)
        for name, _ in entries:
            print(join(directory, name))

    def search(self, keyword, directory="/"):
        if not keyword:
            raise ValueError("Keyword must be non-empty")
        visited = 0
        def walk(path,depth):
            nonlocal visited
            if depth > MAX_DEPTH: raise ValueError("Max search depth exceeded")
            for name, kind in self.entries(path):
                visited += 1
                if visited > MAX_ENTRIES: raise ValueError("Max search entries exceeded")
                full = join(path,name)
                if keyword.casefold() in name.casefold():
                    print(full)
                if kind == 2:
                    walk(full, depth + 1)
        walk(directory,0)

    def disk_space(self):
        info = self.call("info").decode()
        fields = dict(re.findall(r"^\s*(total_blocks|free_blocks|used_blocks|total_vags)\s*:\s*(\d+)", info, re.M))
        for k in ("total_blocks","free_blocks","used_blocks","total_vags"):
            if k not in fields: raise RuntimeError("Missing disk info: "+k)
        total=int(fields["total_blocks"])
        free=int(fields["free_blocks"])
        used=int(fields["used_blocks"])
        vag=int(fields["total_vags"])
        print(f"Total image    : {total * 4096:,} B ({total*4096/1048576:.1f} MiB)")
        print(f"Free alloc     : {free * 4096:,} B ({free*4096/1048576:.1f} MiB)")
        print(f"Used alloc     : {used * 4096:,} B ({used*4096/1048576:.1f} MiB)")
        print(f"VAG count      : {vag}")
        print("Note: free/used are checkpoint allocation counters, not guaranteed new-file capacity.")

    def archive(self, source, dest):
        if self.exists(dest): raise FileExistsError(dest)
        out = io.BytesIO()
        total = 0
        def include(path, archive_name, depth):
            nonlocal total
            if depth > MAX_DEPTH: raise ValueError("Too deep")
            kind=self.kind(path)
            if kind == stat.S_IFDIR:
                out_zip.writestr(archive_name.rstrip("/")+"/", b"")
                for child, _t in self.entries(path):
                    include(join(path,child), archive_name.rstrip("/")+"/"+child,depth+1)
            elif kind == stat.S_IFREG:
                info=self.call("stat",path).decode("utf-8","replace")
                size=re.search(r"^\s*size_bytes\s*:\s*(\d+)",info,re.M)
                if not size or int(size.group(1)) > ZIP_LIMIT-total:
                    raise ValueError("ZIP source exceeds 8 MiB RAM limit")
                data=self.read(path)
                total+=len(data)
                if total > ZIP_LIMIT: raise ValueError("ZIP source exceeds 8 MiB RAM limit")
                out_zip.writestr(archive_name,data)
            else:
                raise ValueError("ZIP archive does not support symlinks in this CLI")
        with zipfile.ZipFile(out,mode="w",compression=zipfile.ZIP_DEFLATED) as out_zip:
            include(source,source.rsplit("/",1)[1],0)
        if out.tell()>ZIP_LIMIT:
            raise ValueError("Compressed ZIP exceeds 8 MiB")
        self.put(dest,out.getvalue())
        print(f"ARCHIVED {total:,} B -> {out.tell():,} B")

    def extract(self, source, target_dir):
        self.ensure_folder(target_dir)
        info=self.call("stat",source).decode("utf-8","replace")
        size=re.search(r"^\s*size_bytes\s*:\s*(\d+)",info,re.M)
        if not size or int(size.group(1))>ZIP_LIMIT:
            raise ValueError("ZIP exceeds 8 MiB")
        blob=self.read(source)
        if len(blob)>ZIP_LIMIT: raise ValueError("ZIP exceeds 8 MiB")
        with zipfile.ZipFile(io.BytesIO(blob)) as archive:
            members=archive.infolist()
            total=sum(m.file_size for m in members)
            if total>ZIP_LIMIT or len(members)>MAX_ENTRIES:
                raise ValueError("Archive too large to extract")
            preflight=[]
            seen=set()
            for member in members:
                filename=member.filename
                if filename.startswith("/") or "\\" in filename:
                    raise ValueError("Unsafe archive path")
                components=[x for x in filename.split("/") if x]
                if not components or any(x in (".","..") for x in components):
                    raise ValueError("Unsafe archive member")
                if ((member.external_attr >> 16) & 0o170000) == stat.S_IFLNK:
                    raise ValueError("ZIP symlink entries are not allowed")
                dest=target_dir
                for comp in components: dest=join(dest,comp)
                if dest in seen:
                    raise ValueError("Duplicate ZIP destination: "+dest)
                seen.add(dest)
                if self.exists(dest): raise FileExistsError(dest)
                preflight.append((member,dest,components))
            # Detect ZIP file entries masquerading as parent directories.
            file_targets={dest for member,dest,_parts in preflight
                          if not member.is_dir()}
            for _member,dest,components in preflight:
                parent=dest.rpartition("/")[0] or "/"
                while parent!=target_dir:
                    if parent in file_targets:
                        raise ValueError("ZIP file conflicts with directory: "+parent)
                    if self.exists(parent) and self.kind(parent)!=stat.S_IFDIR:
                        raise NotADirectoryError(parent)
                    parent=parent.rpartition("/")[0] or "/"
                    if parent=="/" and target_dir!="/":
                        break
            for member,dest,components in preflight:
                cur=target_dir
                for part in components[:-1]:
                    cur=join(cur,part)
                    if not self.exists(cur): self.mkdir(cur)
                if member.is_dir():
                    if not self.exists(dest): self.mkdir(dest)
                else:
                    data=archive.read(member)
                    self.put(dest,data)
        print(f"EXTRACTED {len(members)} entries to {target_dir}")


def parser_create():
    p=argparse.ArgumentParser(
        description="VTXFS terminal file manager; /dev/shm image only")
    p.add_argument("image", help="Existing VTXFS image directly in /dev/shm (tmpfs)")
    sp=p.add_subparsers(dest="command",required=True)
    def add(name,*args):
        q=sp.add_parser(name)
        for arg in args:q.add_argument(arg)
        return q
    add("info"); add("disk-space"); add("check-space"); add("fsck")
    add("ls","path")
    add("open","path"); add("cat","path")
    add("properties","path"); add("stat","path"); add("get-info","path")
    q=add("create","path");q.add_argument("--dir",action="store_true")
    add("mkdir","path")
    add("edit","path")
    add("rename","source","new_name_or_path")
    add("move","source","destination")
    add("copy","source","destination")  # direct copy; clipboard via copy-select
    add("copy-select","source")
    add("cut","source")
    add("paste","directory")
    add("delete","path");add("trash","path")
    q=add("permanently-delete","path");q.add_argument("--yes",action="store_true")
    q=add("empty-trash");q.add_argument("--yes",action="store_true")
    add("restore","entry")
    add("search","keyword").add_argument("--in",dest="directory",default="/")
    q=add("sort","directory");q.add_argument("--by",choices=["name","type","size","mtime"],default="name")
    q=add("filter","directory");q.add_argument("--ext",required=True)
    add("hardlink","source","destination")
    add("symlink","target","link")
    add("shortcut","target","link")
    add("readlink","path")
    q=add("readonly","path");q.add_argument("--off",action="store_true")
    add("hide","path"); add("unhide","path")
    add("zip","source","destination");add("unzip","source","directory")
    add("bitmap","vag")
    return p


def main(argv=None):
    args=parser_create().parse_args(argv)
    v=Volume(args.image)
    cmd=args.command
    if cmd in ("info","fsck","disk-space","check-space"):
        if cmd=="fsck":
            p=subprocess.run([str(BASE/"fsck.vtxfs"),"-q",v.image],
                             capture_output=True,text=True,timeout=120)
            if p.returncode: raise RuntimeError(p.stderr or p.stdout)
            print("FSCK OK")
        elif cmd in ("disk-space","check-space"):v.disk_space()
        else:v.call("info",output=True)
    elif cmd=="ls": v.call("ls",path_ok(args.path),output=True)
    elif cmd in ("cat","open"):
        typ=v.kind(args.path)
        if typ==stat.S_IFDIR: v.call("ls",args.path,output=True)
        elif typ==stat.S_IFLNK: v.call("readlink",args.path,output=True)
        else: v.call("cat",args.path,output=True)
    elif cmd in ("properties","stat","get-info"):
        v.call("stat",path_ok(args.path),output=True)
        if v.kind(args.path)==stat.S_IFLNK:
            print("Link target: "+v.call("readlink",args.path).decode().strip())
    elif cmd=="create": v.create(args.path,args.dir)
    elif cmd=="mkdir": v.create(args.path,True)
    elif cmd=="edit":
        path_ok(args.path,False)
        v.assert_writable(args.path)
        r=subprocess.run([str(BINARY),v.image,"put-stdin",args.path],
                         stdin=sys.stdin.buffer,capture_output=True,timeout=1200)
        if r.returncode:
            raise RuntimeError(r.stderr.decode("utf-8","replace"))
        sys.stdout.buffer.write(r.stdout)
    elif cmd=="rename":
        src=path_ok(args.source,False)
        dst=(args.new_name_or_path if args.new_name_or_path.startswith("/")
             else join(parent_name(src)[0],args.new_name_or_path))
        v.call("move-safe",src,path_ok(dst,False))
    elif cmd=="move": v.call("move-safe",path_ok(args.source,False),path_ok(args.destination,False))
    elif cmd=="copy": v.copy_tree(args.source,args.destination)
    elif cmd in ("copy-select","cut"):
        src=path_ok(args.source,False)
        v.kind(src)
        v.json_put(CLIP, {"source":src,"mode":"cut" if cmd=="cut" else "copy"})
        print(f"Clipboard selected: {src} ({cmd})")
    elif cmd=="paste":
        destdir=path_ok(args.directory)
        if v.kind(destdir)!=stat.S_IFDIR: raise NotADirectoryError(destdir)
        clip=v.json_read(CLIP)
        src=path_ok(clip["source"],False)
        name=src.rsplit("/",1)[1]
        dest=join(destdir,name)
        if clip["mode"]=="cut":
            v.call("move-safe",src,dest)
            v.call("remove",CLIP)
        elif clip["mode"]=="copy":
            v.copy_tree(src,dest)
        else: raise ValueError("Invalid clipboard action")
        print(f"PASTED {src} -> {dest}")
    elif cmd in ("delete","trash"):v.trash(args.path)
    elif cmd=="restore":v.restore(args.entry)
    elif cmd=="permanently-delete":
        if not args.yes:raise ValueError("Add --yes to permanently delete VTXFS entries")
        path=path_ok(args.path,False)
        if path in (TRASH,META,CLIP) or path.startswith(META+"/"):
            raise ValueError("Reserved VTXFS metadata path")
        v.remove_tree(path)
        print("PERMANENTLY REMOVED (logical allocation only, not secure erase): "+path)
    elif cmd=="empty-trash":
        if not args.yes:raise ValueError("Add --yes to empty Trash")
        v.empty_trash()
    elif cmd=="search":v.search(args.keyword,args.directory)
    elif cmd=="sort":v.list_sorted(args.directory,args.by)
    elif cmd=="filter":v.list_sorted(args.directory,ext=args.ext)
    elif cmd=="hardlink":
        src=path_ok(args.source,False)
        dest=path_ok(args.destination,False)
        if v.kind(src)!=stat.S_IFREG:
            raise ValueError("Hard link chỉ hỗ trợ file thường")
        if v.exists(dest):
            raise FileExistsError(dest)
        v.call("hardlink",src,dest)
    elif cmd in ("symlink","shortcut"):
        v.call("symlink",args.target,path_ok(args.link,False))
    elif cmd=="readlink":v.call("readlink",args.path,output=True)
    elif cmd=="readonly":v.call("chmod","0644" if args.off else "0444",args.path)
    elif cmd in ("hide","unhide"):
        path=path_ok(args.path,False)
        parent,name=parent_name(path)
        renamed= ("."+name if cmd=="hide" and not name.startswith(".") else
                   name[1:] if cmd=="unhide" and name.startswith(".") else name)
        if renamed==name: raise ValueError("Already in requested hide state")
        v.call("move-safe",path,join(parent,renamed))
    elif cmd=="zip":v.archive(args.source,args.destination)
    elif cmd=="unzip":v.extract(args.source,args.directory)
    elif cmd=="bitmap":v.call("bitmap",args.vag,"0","64",output=True)


if __name__=="__main__":
    try:main()
    except (ValueError,RuntimeError,FileExistsError,FileNotFoundError,
            NotADirectoryError,PermissionError,KeyError,zipfile.BadZipFile,
            AssertionError,UnicodeError) as exc:
        print(f"vtxfsctl: {type(exc).__name__}: {exc}",file=sys.stderr)
        raise SystemExit(1)
