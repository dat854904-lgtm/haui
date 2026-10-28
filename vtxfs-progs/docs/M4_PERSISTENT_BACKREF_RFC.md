# M4 — RFC: Persistent Reverse-Backreference VTXFS

Trạng thái: **DRAFT / NOT ENABLED**. Đây là đặc tả thiết kế, KHÔNG phải
on-disk implementation. Dữ liệu VTXFS v2.1 và feature bits giữ nguyên.

## Đích

Xác định đầy đủ owner data và metadata của mọi allocation_id, kể cả
snapshot/root chưa được load vào cache, OMAP view trước đó, delayed refs
và root đang reaping. Không được giải phóng block dựa vào chỉ một phần
fs->roots hoặc owner_hint. REF và SFQ vẫn là authority trong v2.1.

## Schema đề xuất v2.2 (chưa cấp numeric type ID)

Một BACKREF_TREE bền vững, key gồm allocation_id (phải fit OID 56-bit),
record kind và collision_id. Value phiên bản có:
- version, record_bytes, kind, flags;
- allocation_id, start_vblk, block_count, birth_xid;
- subvolume/root ID, inode ID và file offset cho data owner;
- parent node OID, tree level, OMAP view XID cho metadata owner;
- reference generation XID, share multiplicity và checksum.

Bắt buộc so sánh đầy đủ owner tuple sau khi tra key hash; không coi hash
là định danh unique. Key overflow, record size và tree level phải được
chứng minh bằng static asserts, endian golden tests và fuzz.

## Transaction protocol

1. Tạo allocation + data CoW trong transaction XID.
2. Cập nhật EXTENT owner, BACKREF owner và REF delta atomically.
3. Draining delayed refs + snapshot pins + OMAP view đến fixed point.
4. REF về zero chỉ được đưa vào SFQ sau khi chắc chắn hết owner/pin.
5. Flush data → meta/ref/backref → VAG → CPD publish/mirror.
6. Recovery chỉ thấy một checkpoint nguyên vẹn, không nửa graph cũ/mới.

## Full-owner enumeration

Quét ROOT_TREE on-disk qua mọi root/snapshot/reaping root, không chỉ
fs->roots cache; pin OMAP_VIEW đúng committed XID rồi duyệt EXTENT
data và metadata tree parent. Reconcile owner multiplicity với
REF và SFQ. Unknown hoặc incomplete parent chain phải -EOPNOTSUPP,
không được giả kết quả complete.

## Migration và compatibility

- Tool upgrade bắt đầu bằng --dry-run và báo owner/count/space overhead.
- Chạy offline trên image riêng, backup, không auto-upgrade khi mount.
- Build tree tạm, chạy cross-tree verifier và crash matrix 7 điểm,
  publish feature INCOMPAT mới sau khi mọi record durable.
- Unknown INCOMPAT phải khiến old reader từ chối RW; không reuse các
  bit đang có chức năng deferred khác.
- Resume/rollback không được ghi đè image v2.1 hay snapshot đang pin.

## Gates bắt buộc

- Snapshot chưa load, reflink, delete/reaper, truncate, CoW metadata.
- Test duplicate/missing references, hash collision, corrupted records,
  overflow, concurrent commit, crash trước/sau owner publication.
- REF == reconciled owner multiplicity theo nghĩa snapshot sharing.
- fsck độc lập xác nhận và reader cũ fail-safe khi gặp feature mới.
- Kernel runtime KUnit/VM và corruption/fault test hoàn chỉnh.

**Trạng thái M4 ngày 2026-10-26:** Có 61 API query/diagnostic compile
trong vtxfs.ko, nhiều API Btrfs-only vẫn trả -EOPNOTSUPP. M4 persistent
backref chưa được triển khai: S0 RFC + S2 đối với API hiện có, KHÔNG S4.
