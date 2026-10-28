# VTXFS v2.1 — M0 on-disk ABI freeze và kiểm thử golden

Trạng thái: **Đã đóng băng ABI v2.1 cho nhánh M0–M3/M5–M6 hiện tại.**
Không nâng feature bits/chuyển format trong các thay đổi kernel, FIEMAP, DIO,
free-space cache, CLI, snapshot và test VM. M4 persistent backref là RFC riêng.

## Đơn vị, định danh và quyền sở hữu

- 4 KiB một block; VAG = 32768 block = 128 MiB.
- VBLK là logical block trong VAG; PBLK là địa chỉ block vật lý của image.
- B+Tree key có OID/type/offset và endian encoded (không cast raw metadata).
- On-disk CPD chọn committed XID duy nhất; Superblock ring + CPD ring,
  mirror tại 64 MiB, CoW tree roots + OMAP view phải phù hợp cùng XID.
- REF là authority của `allocation_id` + `refcount`; SFQ pin block cho đến
  khi `safe_xid` cho phép quay lại VAG bitmap; free-space RB-tree chỉ là cache.
- Không mang trực tiếp Btrfs TREE_BLOCK_REF hay APFS spaceman item lên image.

## Feature gates hiện hành

Các bit `INCOMPAT` cần thiết: `V21_LAYOUT`, `OMAP_VIEWS`, `EXTENT_CSUM`.
P0 không cho phép các bit deferred (`MULTIDEV`, `FAST_WAL`, `AEAD`,
`DUP_META`, `COMPRESS`, `SEND_RECEIVE`,...). Unknown INCOMPAT phải từ
chối mount, không viết/repair như thể format cũ.

**M4 không dùng lại bit cũ:** một feature bit INCOMPAT mới cho
persistent backref phải được cấp sau khi RFC, migration, verifier, crash
tests hoàn thành. Không tự đổi `uapi/vtxfs.h` trước các gate này.

## Golden contract tests

```sh
cd /home/haui/Music/Downloads/VTXFS
python3 tests/test_m0_golden.py
tools/vtxfs-dev-check m0
```

Tạo image 256MiB riêng trong `/dev/shm`, format mới, fsck, tạo thư mục,
ghi và đọc nội dung nhị phân đúng SHA-256, snapshot, fsck lại. In JSON
manifest theo semantic contract. Đây là **semantic golden**, chưa phải
corpus nhị phân cố định đa generation; cần bổ sung golden image v2.1 với
pinning hash trước khi bất kỳ migration v2.2 nào được phép.

Rollback code: có backup trong
`.vtxfs-backup-20261026/m0_m6_20261026/`. Không tự động áp dụng thay đổi
metadata lên image hiện hữu của người dùng.
