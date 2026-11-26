# Kế hoạch Memory Management — demacOprx

Cập nhật: 26/11/2026.
- Workspace: `/home/minhbanngay/Study/BTL_OS1`.
- Source repo: `Assignment-OS-CO2017/`, branch `py-mem`.
- Bản main đã audit: `f05c748`.
- Bản sửa đã push lên `upstream/py-mem`: `59858303899ab9fcbcdef4c25b04b22a0c209f2f` — Fix swap device wiring and safe default RAM size.
- Local py-mem đã fast-forward tới main trước khi sửa. Chưa xác nhận commit 5985830 được merge vào main.
- Đối chiếu: rules.md, MM_OS.xlsx, source tích hợp và baocao_mm.pdf phiên bản 22/11/2026.

## 1. Quy ước

- [x]: có implementation; bằng chứng kiểm thử cụ thể ở mục 6.
- [~]: có code nhưng còn hạn chế/lỗi hoặc chưa xác minh đầy đủ.
- [ ]: việc còn phải thực hiện.
- Có code/build thành công không đồng nghĩa đã kiểm thử toàn diện.
- Các mục ĐỀ XUẤT trong Excel vẫn cần nhóm duyệt; implementation không chứng minh đã được phê duyệt.

## 2. Tiến độ theo giai đoạn

| Giai đoạn | Trạng thái |
| --- | --- |
| 1. Physical memory | Lõi đã hoàn thành; có thêm bitmap, contiguous allocation và verify. MEMPHY_dump còn TODO. |
| 2. Read/Write qua MMU | Đã có implementation thật và kiểm tra region dưới MM lock. Không còn chờ dummy PTE như ngày 20/11. Còn lỗi register bounds/error propagation ở CPU/wrapper. |
| 3. Swap | Đã có swap-out/in, cập nhật PTE, trả slot. Wiring thiết bị đã sửa; test round-trip với hai frame RAM PASS. |
| 4. Reclaim/replacement | Đã có ba tầng và FIFO. Cần test sâu OOM/SWAP đầy/rollback và xác nhận scope policy với nhóm/giảng viên. |
| 5. Cleanup/DMA | Cleanup RAM/SWAP đã qua test. DMA có submit bất đồng bộ nhưng còn thiếu pin trang/lifetime khi reclaim hoặc process thoát. |

## 3. Người 1 — Page Table/MMU và tích hợp MM

Branch PTE tại e855c0c đã nằm trong main tại thời điểm audit.

- [x] Canonical 57-bit, page 4 KiB, năm index địa chỉ.
- [x] Cây page table năm cấp thật, cấp bảng trung gian động; không còn dummy PTE ở các API cốt lõi.
- [x] pt_walk_internal, pte_get_entry, pte_set_entry, pte_set_fpn, pte_set_swap.
- [x] Trạng thái EMPTY/PRESENT/SWAPPED, mã PT_STATUS_*.
- [x] pt_translate_va, pt_translate_user_for_copy, pt_update_access_flags.
- [x] pt_unmap_one, pt_unmap_kernel_one, pruning, pt_destroy_tree.
- [x] alloc_pages_range, vmap_page_range, vm_map_ram và kernel mapping đã có code rollback.
- [x] pt_swapin_commit, mapping iterator theo process/toàn hệ thống.
- [x] Kernel/user copy, zero range và thống kê page table đã có implementation.
- [~] Chưa chạy lại toàn bộ failure-injection test cho rollback/map/unmap theo bộ test của báo cáo.

Cần phối hợp tiếp:

- [ ] Kiểm thử map/unmap/swap-in thất bại, bảo toàn mapping cũ khi OOM.
- [ ] Chốt cơ chế pin hoặc lifetime mapping cho DMA bất đồng bộ.
- [ ] Đối chiếu bộ đếm hiện có với yêu cầu số lần truy cập bộ nhớ trong output/báo cáo, không chỉ số lần walk và dung lượng page table.

API đề xuất trong plan cũ đã có chức năng tương đương; không cần thêm lại chỉ để khớp tên pte_lookup/pte_map_page.

## 4. Người 2 — VMA/Syscall/Kernel

Bản tích hợp phát triển thêm trên p2_memory (c193c0c). Branch p2_update (94160e9) có sửa registry/lock nhưng chưa là ancestor của main khi audit.

- [x] Region active, kiểm tra rgid, PID registry và syscall resolve PCB thật.
- [x] Layout macOS-style, VMA SMALL/MEDIUM/LARGE, tìm vùng trống và tăng sbrk.
- [x] ALLOC eager/rollback, FREE lazy, free-region coalescing và zero vùng tái sử dụng đã có implementation.
- [x] Đường access dùng pg_region_va/mm_region_is_active, kiểm region và offset.
- [x] Kernel VA allocator, pools và DIRECT allocation đã có implementation.
- [x] registry_foreach_process chụp danh sách process cho reclaim/iterator.
- [x] Cleanup gọi kmem_release_process/free_pcb_memph trước khi hủy VMA/mm.
- [~] Thứ tự khóa được tài liệu hóa: mmvm_lock → os_mm_lock → kmm_lock; lifetime/race cần kiểm thử sâu.
- [~] Registry main vẫn xóa slot thành NULL, có thể làm lookup bỏ sót PID collision.

Cần bổ sung:

- [ ] Tích hợp chọn lọc tombstone từ p2_update; cập nhật các hàm duyệt để bỏ qua tombstone.
- [ ] Kiểm tra lifetime PCB khi lookup/snapshot chạy đồng thời cleanup.
- [ ] Không merge mù API lock: mm_global_lock ở p2_update khóa os_mm_lock, khác mmvm_lock bảo vệ MM hiện tại.
- [ ] Chặn register index >= 10 ở liballoc, libkmem_malloc và READ trong cpu.c trước khi ghi regs[10].
- [ ] READ chỉ cập nhật thanh ghi khi thành công; CPU xử lý/báo lỗi run() thay vì bỏ qua.
- [ ] Hoàn thiện validation cấu hình explicit và thông báo lỗi cho kích thước RAM/SWAP không hợp lệ/vượt giới hạn.

## 5. Người 3 — RAM/Read–Write/Swap

Phần physical-memory ban đầu do Người 3 làm; nhiều mục thiếu trong plan 20/11 đã được P1/nhóm triển khai khi tích hợp. Không ghi công toàn bộ phần tích hợp cho Người 3.

- [x] MEMPHY_format/get_freefp/put_freefp, frame 0, bounds và double free.
- [x] MEMPHY_read/write, MEMPHY_seq_read/write, MEMPHY_mv_csr.
- [x] init_memphy: zero-init, swap 0 byte, lỗi allocation/format, giới hạn INT_MAX.
- [x] __swap_cp_page copy 4096 byte và lan truyền lỗi.
- [x] Bản tích hợp thêm numfp, nfree, fp_bitmap, MEMPHY_verify và contiguous frame APIs.
- [x] pg_getpage, pg_getval/setval, __read/write, libread/write đã có implementation.
- [x] __read_user_mem/__write_user_mem dùng đường user access có kiểm tra.
- [x] Slot SWAP dùng MEMPHY_get_freefp/put_freefp trên thiết bị tương ứng; không cần thêm swap_slot_alloc/free riêng.
- [x] pg_swapout điều phối copy/PTE/rollback; pg_getpage swap-in rồi commit trước khi trả slot.
- [x] Reclaim dùng mm_reclaim_global_user, mm_reclaim_kernel_cache, mm_swap_active_user; thay các helper đề xuất cũ.
- [x] find_victim_page đã sửa FIFO một node.
- [x] free_pcb_memph dùng iterator/unmap/cây năm cấp; cleanup RAM/SWAP đã qua regression test.
- [ ] MEMPHY_dump vẫn TODO.
- [~] Hiện chọn một active SWAP; chưa tự chuyển sang device khác khi active SWAP đầy.

### Bản sửa đã push 26/11 — commit 5985830

- [x] Bỏ ép kiểu sai mảng struct thành struct memphy_struct **.
- [x] Mảng con trỏ SWAP thật nằm trong loader args, giữ lifetime xuyên suốt mô phỏng.
- [x] Device 0 byte/không có frame đầy đủ không được chọn active.
- [x] Chọn device đầu tiên dùng được, truyền đúng active_mswp_id sang kernel.
- [x] Kiểm tra lỗi khởi tạo, dừng với lỗi nếu không có active SWAP dùng được.
- [x] Swap-in tra đúng device từ PTE; thiếu device trả lỗi trước reclaim, không fallback nhầm device.
- [x] RAM mặc định đổi từ 4 GiB vượt INT_MAX sang PAGING_MEMRAMSZ = 2 MiB; SWAP giữ 16 MiB.
- [x] Input explicit vẫn dùng kích thước khai báo; init_memphy tiếp tục từ chối giá trị vượt INT_MAX.
- [x] Commit/push lên upstream/py-mem.

File trong commit: src/os.c, src/libmem.c, test/swap_roundtrip.c, test/run_swap.sh.

## 6. Kiểm thử đã thực hiện

Các kiểm tra dưới đây chạy trên source sau đó được commit thành 5985830.

- [x] make all thành công, gồm Verilator RTL.
- [x] RAM hai frame, cấp ba trang; ba vòng đọc lại và đối chiếu từng byte, buộc swap-in/out.
- [x] Lặp kịch bản khi chỉ bật SWAP 0, SWAP 1 hoặc SWAP 3.
- [x] Thiếu device trong bảng: read trả lỗi, giữ output, không reclaim thêm frame.
- [x] Tất cả SWAP bị vô hiệu hóa: khởi tạo trả lỗi.
- [x] Cleanup trả hết frame/slot; MEMPHY_verify trả 0.
- [x] ASan/UBSan cho regression test; leak detection tắt do giới hạn ptrace của môi trường.
- [x] Sample os_syscall_list, os_1_singleCPU_mlq_paging, os_2_mlq_paging kết thúc exit 0.
- [x] Sau sửa default: legacy sched, os_1_singleCPU_mlq và explicit os_2_mlq_paging kết thúc exit 0.
- [x] git diff --check sạch trước commit.
- [ ] Chưa chạy lại make dma/test DMA cho commit này.
- [ ] Chưa tái xác minh bộ 30 test và 20 lần chạy nêu trong baocao_mm.pdf. Checkout remote đã audit không chứa bộ test đó; thư mục tests đang bị ignore.
- [ ] Chưa coi exit 0 của sample là bằng chứng mọi lệnh thành công: CPU hiện bỏ qua lỗi run().

Chạy regression test:

```bash
cd /home/minhbanngay/Study/BTL_OS1/Assignment-OS-CO2017
sh test/run_swap.sh
ASAN_OPTIONS=detect_leaks=0 \
  SWAP_TEST_CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  sh test/run_swap.sh
```

## 7. Việc tiếp theo theo ưu tiên

1. [ ] Review/merge commit 5985830 từ py-mem vào main.
2. [ ] Sửa register bounds và error propagation; test index 9, 10 và READ lỗi không đổi register.
3. [ ] Tích hợp registry tombstone; test collision → unregister → lookup và lifetime PCB/lock.
4. [ ] Người 3 mở rộng test SWAP đầy, rollback giữa transaction, nhiều process, lỗi thiết bị; kiểm tra bitmap/list/PTE và dữ liệu cũ.
5. [ ] Chốt dùng một active SWAP hay chuyển device khi đầy; nếu mở rộng phải lưu/cleanup đúng swap type.
6. [ ] Phối hợp DMA pin/unpin hoặc lifetime transaction; chứng minh CPU chạy tiếp và page không bị reclaim/cleanup sớm.
7. [ ] Hoàn thiện MEMPHY_dump nếu cần demo/báo cáo; kiểm tra thống kê truy cập và page-table storage.
8. [ ] Đưa bộ test báo cáo lên repo hoặc cung cấp runner tái lập; chạy lại bộ tích hợp trước demo.

## 8. Quy tắc giữ nguyên

- Canonical 57-bit, page 4 KiB, năm cấp paging và layout macOS theo rules.md.
- FPN 0 hợp lệ; bitmap, list và PTE phải nhất quán.
- CPU đọc/ghi qua MMU; trang SWAPPED phải nạp về RAM trước access.
- User root riêng cho mỗi process; kernel root dùng chung qua struct krnl_t.
- Lỗi allocation/swap không làm mất mapping và dữ liệu cũ hợp lệ.
- Địa chỉ vật lý cho DMA phải ổn định suốt transfer, không chỉ lúc tra địa chỉ.
- Hợp đồng cross-team ghi rõ address domain, mã lỗi, lock ownership và trách nhiệm rollback/cleanup.
