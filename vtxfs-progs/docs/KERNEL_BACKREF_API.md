# Backref VTXFS (kernel) — chức năng đã bổ sung và giới hạn Btrfs

## Phạm vi

Đã mở rộng `kernel/backref.c` và `kernel/vtxfs_backref.h` bằng API có
tiền tố **vtxfs_**. Đây là thuật toán dựa trên ABI **VTXFS v2.1**,
không liên kết mã Btrfs hoặc giải mã metadata Btrfs. Prototype mới
được include từ `kernel/vtxfs_subsys.h`. Không sửa định dạng đĩa.

**Cực kỳ quan trọng**: VTXFS có VTX_TYPE_EXTENT theo (inode, file_offset),
`allocation_id` bất biến, VTX_TYPE_REF theo `allocation_id` với
`refcount`, và VTX_TYPE_INODE_REF theo (inode,parent_ino). Nó **không**
có Btrfs TREE_BLOCK_REF/SHARED_BLOCK_REF, INODE_EXTREF hay inline
tree/data backref item kiểu Btrfs. Những hàm đòi hỏi loại metadata
đó trả `-EOPNOTSUPP`; KHÔNG được dùng các suy đoán owner cho block
free, SFQ hay reaper.

**Điều kiện khóa**: gọi các hàm duyệt FS/REF tree với
`fs->trans_lock` đang giữ (được kiểm bởi lockdep). Danh sách kết quả
được cấp phát bằng `GFP_NOFS`; caller phải giải phóng qua helper phù
hợp. Không nạp vtxfs.ko hay mount block device trong quá trình sửa.

## Nhóm có xử lý thật

| Các hàm (bỏ tiền tố vtxfs_) | Chi tiết |
| --- | --- |
| `check_extent_in_eb`, `find_extent_in_eb` | Xác minh node/slot/độ dài item trước khi đọc allocation_id |
| `iterate_leaf_refs` | Duyệt FS-tree và chỉ gọi callback trên EXTENT thực |
| `find_extent_backrefs` | Quét các root **đã nạp trong fs->roots**, lọc cùng allocation_id, tạo danh sách owner |
| `free_backrefs`, `free_inode_elem_list` | Thu hồi kết quả duyệt đã cấp phát |
| `extent_is_shared`, `is_data_extent_shared` | Tra REF tree có refcount > 1, không suy luận chỉ theo thời gian XID |
| `extent_from_logical` | Tìm extent bao phủ (inode, file offset) |
| `tree_backref_for_extent` | Tra `vtx_ref_item` của allocation_id; KHÔNG tạo Btrfs tree parent info |
| `prelim_ref_init`, `prelim_ref_compare`, `prelim_ref_rb_add_cmp`, `prelim_ref_insert` | RB-tree tạm gom các ref theo (alloc,subvol,inode,offset,direct/indirect) |
| `add_prelim_ref`, `add_direct_ref`, `update_share_count`, `free_pref`, `prelim_release` | Insert, cộng delta có kiểm tra overflow, giải phóng |
| `is_shared_data_backref`, `lookup_backref_shared_cache`, `store_backref_shared_cache` | Kết quả tạm **theo một query**; tuyệt đối không dùng làm quyết định thu hồi block |
| `iterate_extent_inodes`, `iterate_inodes_from_logical`, `build_ino_list` | List/callback inode của allocation_id từ các root hiện đang trong cache |
| `unode_aux_to_inode_list`, `add_all_parents` | Chuyển preliminary refs hoặc INODE_REF thành danh sách inode/parent |
| `iterate_inode_refs`, `inode_to_path`, `ref_to_path`, `paths_from_inode` | Tìm parent từ VTXFS INODE_REF và dựng đường dẫn, chặn chu trình/quá sâu |
| `backref_iter_init`, `backref_iter_start`, `backref_iter_next`, `backref_iter_release` | Iterator duyệt owner refs, tự thu hồi |
| `find_all_leafs`, `free_leaf_list` | Gom các extent buffer chứa allocation_id, giữ EB ref và giải phóng đúng |
| `init_data_container`, `init_ipath`, `free_path_records` | Vòng đời container và path record |
| `backref_error_cleanup`, `backref_init_cache` | Thu hồi hoặc reset cache theo query khi an toàn |

`backref_iter_is_inline_ref` luôn false vì VTXFS không lưu
inline **backreference**; nó không có nghĩa VTXFS không có dữ liệu
inline trong file.

## Nhóm chỉ có API báo KHÔNG HỖ TRỢ (-EOPNOTSUPP)

- `add_indirect_ref`, `resolve_indirect_ref`, `resolve_indirect_refs`,
  `add_missing_keys`, `find_parent_nodes`: thiếu tree parent/bytenr
  encoding cùng mức độ chia sẻ root của Btrfs.
- `add_inline_refs`, `add_keyed_refs`, `get_extent_inline_ref`,
  `iterate_inode_extrefs`, `find_one_extref`: các loại record này không
  tồn tại trong VTXFS v2.1.
- `add_delayed_refs`: `vtxfs_delayed_ref` có allocation/start/count,
  không có danh tính inode/parent cần thiết để suy ngược owner.
- `find_all_roots_safe`: `fs->roots` chỉ chứa các root được load; chưa
  có bộ duyệt root-tree / snapshot metadata đầy đủ trong backref.
- `handle_direct_tree_backref`, `handle_indirect_tree_backref`,
  `backref_add_tree_node`, `backref_finish_upper_links`,
  `backref_alloc_node`, `backref_free_node`,
  `backref_drop_node`, `backref_cleanup_node`:
  Btrfs graph cache/upper links chưa có layout tương ứng. Các API này
  KHÔNG giả tạo graph rồi báo thành công.

Chuỗi `void btrfs` trong yêu cầu không phải tên prototype C hợp lệ;
không tạo biểu tượng `btrfs_*` trong namespace của VTXFS.

## Giới hạn liên kết và thực thi

**Các API backref này hiện là API truy vấn/chẩn đoán**, chưa được nối
vào nhánh giải phóng block hoặc reaper. Không nên nối vào đường
giải phóng block trước khi có độ bao phủ đầy đủ tất cả snapshot, các
root chưa load và các delayed refs. Không tìm thấy hàm trong một lượt
ftrace không chứng minh hàm không được dùng.

`find_extent_backrefs` chỉ quét root trong `fs->roots`, do đó không
chứng minh đã liệt kê TẤT CẢ owner trên disk. Kết quả `is_shared...`
của preliminary cache cũng không phải REF authoritative.

## Test đã thực hiện

```bash
cd /home/haui/Music/Downloads/VTXFS
make -C /lib/modules/$(uname -r)/build M="$PWD" modules -j2
make -C /lib/modules/$(uname -r)/build M="$PWD" \
  kernel/kunit/vtxfs_backref_test.o -j2
python3 tests/test_kernel_backref_contract.py
python3 tests/instrument_kernel_calls.py
python3 tests/audit_kernel_symbols.py
python3 tests/test_terminal_full_matrix.py
TMPDIR=/dev/shm bash tests/run.sh
```

Kiểm tra ABI/symbol: **61 API có khai báo và symbol tương ứng trong
vtxfs.ko**. KUnit test object có test so sánh/gom prelim refs và
-EOPNOTSUPP nhưng chỉ được **biên dịch**, chưa được load/chạy trên kernel.
Userspace CLI test chạy trên image tmpfs không kích hoạt kernel/backref.c.
Vì vậy **chưa thể khẳng định backref kernel đã vượt qua runtime tests**.

Backup mã cũ: `.vtxfs-backup-20261026/kernel_backref_before_expand.c`.
