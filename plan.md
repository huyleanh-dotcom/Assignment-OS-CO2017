# Kế hoạch triển khai Memory Management — demacOprx

Cập nhật: 20/11/2026  
Workspace: `/home/minhbanngay/Study/BTL_OS1`  
Source repo: `Assignment-OS-CO2017/`

## 1. Quy ước trạng thái

- `[x]`: đã cài đặt, có unit test và `make all` thành công trên workspace hiện tại.
- `[~]`: đã có code/khung nhưng chưa đủ điều kiện tích hợp hoặc chưa đáp ứng đầy đủ đặc tả.
- `[ ]`: chưa làm.
- API ghi **ĐỀ XUẤT** trong `MM_OS.xlsx` chỉ được thêm sau khi cả nhóm duyệt hợp đồng.

## 2. Kết luận tiến độ hiện tại

- **Giai đoạn 1 — Physical memory cơ bản:** phần lõi đã hoàn thành. `MEMPHY_dump()` là công cụ chẩn đoán P2/M4 nên chưa tính là điều kiện hoàn thành lõi của giai đoạn này.
- **Giai đoạn 2 — Read/Write qua MMU:** chưa thể triển khai hoàn chỉnh vì API PTE 5 cấp của Người 1 chưa hoạt động và API kiểm tra access/lock của Người 2 chưa đủ.
- **Giai đoạn 3 — Swap:** mới hoàn thành primitive copy một page 4 KiB (`__swap_cp_page`). Chưa có swap-slot allocator, swap-in/out state machine hoặc cập nhật PTE thật.
- **Giai đoạn 4 — Reclaim/replacement:** chưa bắt đầu; các helper chính vẫn là **ĐỀ XUẤT**, replacement policy còn chờ giảng viên/nhóm chốt.
- **Giai đoạn 5 — Cleanup/DMA:** chưa hoàn thành.

## 3. Người 3 — RAM / Read–Write / Swap

### 3.1. Đã hoàn thành

- [x] `MEMPHY_format`
  - Tạo frame 4 KiB theo MM64.
  - FPN 0 hợp lệ.
  - Khởi tạo `fp_next`, `owner`, free/used list.
  - Device 0 byte hoặc nhỏ hơn một page có danh sách frame rỗng.
  - Rollback node nếu cấp phát metadata thất bại.
- [x] `MEMPHY_get_freefp`
  - Lấy node khỏi free list, trả FPN và chuyển node sang used list.
  - Không làm mất metadata frame.
- [x] `MEMPHY_put_freefp`
  - Chuyển node thật từ used list về free list.
  - Xóa owner; từ chối invalid FPN và double free.
- [x] `MEMPHY_read`
  - Kiểm tra device, storage, output pointer và bounds.
- [x] `MEMPHY_write`
  - Kiểm tra device, storage và bounds; không ghi khi lỗi.
- [x] `init_memphy`
  - Trạng thái khởi tạo xác định, storage zero-initialized.
  - Hỗ trợ swap 0 byte bị vô hiệu hóa.
  - Xử lý lỗi allocation/format.
  - Từ chối kích thước vượt `INT_MAX` vì `maxsz` hiện là `int`.
- [x] `MEMPHY_seq_read`
- [x] `MEMPHY_seq_write`
- [x] `MEMPHY_mv_csr`
  - Sửa điều kiện `rdmflg` bị đảo và kiểm tra cursor/bounds.
- [x] `__swap_cp_page`
  - Copy đúng một page 4096 byte.
  - Preflight device/FPN/bounds và tránh overflow phép nhân địa chỉ.
  - Lan truyền lỗi `MEMPHY_read/write`.
- [x] Unit test cho physical-memory core và page copy.
- [x] ASan/UBSan cho các đường đã test (leak detection không khả dụng trong môi trường ptrace hiện tại).
- [x] Full build `make all`, gồm Verilator RTL.

### 3.2. Chưa làm hoặc đang bị chặn

- [ ] `pg_getpage`: cần PTE lookup/state API thật của Người 1.
- [ ] `pg_getval`, `pg_setval`: cần `pg_getpage`, dirty-bit contract và lock.
- [ ] `__read`, `__write`, `libread`, `libwrite`: cần API validate access của Người 2 và PTE của Người 1.
- [ ] `__read_user_mem`, `__write_user_mem`: cần phân quyền, cross-page handling và PTE thật.
- [ ] `swap_slot_alloc`, `swap_slot_free` — **ĐỀ XUẤT**, cần nhóm duyệt.
- [ ] `__mm_swap_page`: hiện mới là copy một chiều; chưa có transaction resident ↔ swapped.
- [ ] Swap-in trong `pg_getpage`.
- [ ] `mem_alloc_frame_with_pressure`, `mem_reclaim_global`, `mem_reclaim_candidate_scan` — **ĐỀ XUẤT**.
- [ ] `find_victim_page`: policy FIFO/global/local chưa chốt; code hiện tại có lỗi với danh sách một node.
- [ ] `free_pcb_memph`: code legacy một cấp, chưa phù hợp page table MM64 5 cấp.
- [ ] `MEMPHY_dump`, các hàm in danh sách phục vụ báo cáo.
- [ ] `libmem_dma_memcpy`: chờ chốt DMA dùng VA, PA hay bus address.

## 4. Người 1 — Page Table / MMU

Nguồn đã xem: branch `upstream/PTE`, commit `03c7dcf`.

### 4.1. Đã có

- [x] Macro/index cơ bản cho địa chỉ MM64 trong `mm64.h`.
- [x] `mm_is_canonical_va` cho canonical 57-bit.
- [x] `get_pd_from_address` tách năm index PGD/P4D/PUD/PMD/PT.
- [x] `get_pd_from_pagenum`.
- [x] Khung encode/decode PTE 64-bit:
  - present;
  - swapped;
  - FPN;
  - swap type/offset;
  - permission/access-dirty fields.
- [~] Có prototype và body khung cho `pte_get_entry`, `pte_set_entry`, `pte_set_fpn`, `pte_set_swap`, `alloc_pages_range`, `vm_map_ram`.

### 4.2. Người 1 cần bổ sung

- [ ] Cây page table 5 cấp thật, cấp bảng trung gian động, 512 entry/table.
- [ ] `pt_walk_internal` hoặc API tương đương, hỗ trợ lookup và create-on-demand.
- [ ] Sửa `pte_get_entry`: hiện trả địa chỉ dummy/local, không đọc leaf PTE thật.
- [ ] Sửa `pte_set_entry`, `pte_set_fpn`, `pte_set_swap`: hiện ghi dummy allocation, không cập nhật cây thật.
- [ ] Mã trạng thái rõ ràng cho empty/present/swapped/invalid.
- [ ] Dirty-bit getter/setter dùng được bởi `pg_setval`.
- [ ] `pt_unmap_one` trả old PTE/FPN để Người 3 reclaim an toàn.
- [ ] `pt_prune_empty` để xóa PT/PMD/PUD/P4D rỗng nhưng giữ root.
- [ ] `pt_destroy_tree` cho cleanup process.
- [ ] `alloc_pages_range` phải atomic, rollback toàn bộ frame/PTE nếu thiếu frame.
- [ ] `vmap_page_range`/`vm_map_ram` phải map thật và rollback khi lỗi.
- [ ] Thống kê page-table storage và memory-access count cho báo cáo.
- [ ] Chốt PTE bit layout và kiểu intermediate entry với cả nhóm (**STOP 1** trong bảng quyết định).

### 4.3. Hợp đồng tối thiểu Người 3 cần từ Người 1

```c
int pte_lookup(struct mm_struct *mm, addr_t va, mm64_pte_t *out);
int pte_map_page(struct mm_struct *mm, addr_t va,
                 addr_t fpn, uint64_t flags);
int pte_mark_swapped(struct mm_struct *mm, addr_t va,
                     int swap_type, addr_t swap_slot);
int pte_set_dirty(struct mm_struct *mm, addr_t va);
int pte_unmap_page(struct mm_struct *mm, addr_t va,
                   mm64_pte_t *old_pte);
int pt_destroy_tree(struct mm_struct *mm);
```

Tên API có thể thay đổi, nhưng phải chốt address domain, mã lỗi, lock ownership và rollback responsibility.

## 5. Người 2 — VMA / Syscall / Kernel

Nguồn đã xem: branch `upstream/p2_memory`, commit `c193c0c`.

### 5.1. Đã có

- [x] Thêm trạng thái `active` cho region.
- [x] Sửa kiểm tra biên `rgid` trong `get_symrg_byid`.
- [x] Có `vm_validate_active_region` mức cơ bản.
- [x] Có `vm_page_has_active_region` để phát hiện active region chồng lên page.
- [x] Có PID registry:
  - `registry_register`;
  - `registry_lookup`;
  - `registry_unregister`.
- [x] `sys_mem` bắt đầu resolve PCB thật từ PID thay vì dummy PCB.
- [~] `__alloc`, `__free` đã quản lý `active` và free-region tốt hơn main nhưng còn TODO/rollback chưa hoàn chỉnh.
- [~] Có khung kernel heap/cache pool và centralized cleanup.
- [~] Có `os_mm_lock`, `mmvm_lock`, `kmm_lock`, nhưng hợp đồng lock chưa thống nhất.

### 5.2. Người 2 cần bổ sung

- [ ] API validate một memory access đầy đủ, không chỉ kiểm tra region active:
  - `vmaid`, `rgid`;
  - `offset` và `access_size`;
  - integer overflow;
  - địa chỉ cuối cùng thuộc user space;
  - trả `out_vaddr` đã xác thực.
- [ ] Đưa `vm_validate_active_region` và `vm_page_has_active_region` vào public header để Người 3 dùng hợp lệ.
- [ ] API duyệt registry an toàn cho global reclaim.
- [ ] Bảo đảm lifetime PCB: `registry_lookup` hiện thả lock trước khi caller dùng pointer, có nguy cơ use-after-free.
- [ ] Sửa open-address registry deletion/lookup hoặc dùng tombstone; không được dừng sai tại slot `NULL` sau khi đã xóa một collision entry.
- [ ] Thống nhất một global MM lock hoặc quy định lock ordering rõ ràng.
- [ ] Expose lock/transaction API; tránh để Người 3 tự khóa lại non-recursive mutex.
- [ ] Hoàn thành atomic `ALLOC`: chỉ commit region/VMA sau khi map đủ frame; rollback khi lỗi.
- [ ] Hoàn thành `FREE` lazy semantics, split/coalesce free list và zero-init vùng tái sử dụng.
- [ ] Hoàn thành syscall PID contract, error propagation và unique syscall registration.
- [ ] Cleanup order phải gọi Người 3 trả RAM/swap trước, rồi Người 1 hủy page table, sau đó mới free VMA/mm/PCB.
- [ ] Đọc đúng `KERNEL_BASE_ADDR`/memory-size config và giữ tương thích sample input.

### 5.3. Hợp đồng tối thiểu Người 3 cần từ Người 2

```c
int vm_validate_active_access(struct pcb_t *caller,
                              int vmaid, int rgid,
                              addr_t offset, addr_t size,
                              addr_t *out_vaddr);

int registry_for_each_locked(struct krnl_t *krnl,
                             int (*callback)(struct pcb_t *, void *),
                             void *arg);

int mm_global_lock(void);
int mm_global_unlock(void);
```

Có thể thay bằng callback/transaction API khác nếu bảo đảm lookup + access + reclaim không race và không tạo recursive locking.

## 6. Điểm tích hợp giữa ba người

- Branch `PTE` và `p2_memory` cùng sửa `include/mm.h` và `src/mm64.c`; cần merge/rebase có review, không chép đè file.
- Chưa triển khai Read/Write/Swap state machine cho đến khi chốt:
  1. PTE representation và state codes.
  2. Address domain của từng API (VA/page number/FPN/physical byte/bus address).
  3. Global MM lock ownership và lock ordering.
  4. Error codes.
  5. Cleanup/rollback responsibility.
- FPN 0 luôn là frame hợp lệ.
- User `READ/WRITE` phải đi qua `caller->mm`; kernel memory đi qua `struct krnl_t`/kernel root.
- SWAP không được CPU truy cập trực tiếp: phải swap-in vào RAM trước khi user read/write.

## 7. Thứ tự công việc tiếp theo

1. [ ] Nhóm duyệt PTE layout, intermediate-table representation và lock contract.
2. [ ] Người 1 hoàn thiện page-table walk + lookup/map/swap/unmap.
3. [ ] Người 2 hoàn thiện validate-access + registry iteration/lifetime + cleanup order.
4. [ ] Người 3 triển khai `pg_getpage`, `pg_getval`, `pg_setval`.
5. [ ] Người 3 nối `__read`, `__write`, `libread`, `libwrite` dưới global MM lock.
6. [ ] Nhóm duyệt `swap_slot_alloc/free`; Người 3 triển khai swap-in/out transaction.
7. [ ] Nhóm chốt reclaim/replacement; triển khai global reclaim trước, replacement sau nếu được yêu cầu.
8. [ ] Tích hợp cleanup process và test đa CPU/race.
9. [ ] Chốt DMA address semantics rồi mới nối `libmem_dma_memcpy`.
10. [ ] Thu thập memory-access count, page-table storage và output phục vụ báo cáo.

## 8. Kiểm thử bắt buộc trước khi đánh dấu hoàn thành

- Unit test MM core và page-table API.
- `make all` và `make dma`.
- Sample inputs cũ và explicit memory-size inputs.
- RAM 0/nhỏ hơn page/đúng page/nhiều page; swap device 0 byte.
- FPN 0, hết frame, rollback allocation, invalid/double free.
- Read/write đầu-cuối page và cross-page.
- Swap-in/out khi RAM đầy, SWAP đầy và lỗi giữa transaction.
- Multi-CPU: translation/access không race với reclaim/cleanup.
- DMA: CPU chạy việc khác trong khi transfer diễn ra; completion qua interrupt.

