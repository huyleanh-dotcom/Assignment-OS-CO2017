/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

//#ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Virtual memory module mm/mm-vm.c
 */

#include "string.h"
#include "mm64.h"
#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

/*get_vma_by_num - get vm area by numID
 *@mm: memory region
 *@vmaid: ID vm area to alloc memory region
 *
 */
struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid)
{
  /* NULL-safe walk: match by id, stop at end of list
   * (ban goc deref pvma->vm_next khi NULL -> segfault) */
  if (mm == NULL)
    return NULL;

  struct vm_area_struct *pvma = mm->mmap;

  while (pvma != NULL)
  {
    if (pvma->vm_id == (unsigned long)vmaid)
      return pvma;
    pvma = pvma->vm_next;
  }

  return NULL;
}

int __mm_swap_page(struct pcb_t* caller, addr_t vicfpn, addr_t swpfpn)
{
    if (caller == NULL || caller->krnl == NULL)
        return -1;
    return __swap_cp_page(caller->krnl->mram, vicfpn, caller->krnl->active_mswp, swpfpn);
}

/*get_vm_area_node - get vm area for a number of pages
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 *@vmastart: vma end
 *@vmaend: vma end
 *
 */
struct vm_rg_struct *get_vm_area_node_at_brk(struct pcb_t *caller, int vmaid, addr_t alignedsz)
{
  struct vm_rg_struct * newrg;
  /* TODO retrive current vma to obtain newrg, current comment out due to compiler redundant warning*/
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (cur_vma == NULL)
    return NULL;

  newrg = malloc(sizeof(struct vm_rg_struct));
  if (newrg == NULL)
    return NULL;

  /* TODO: update the newrg boundary
  // newrg->rg_start = ...
  // newrg->rg_end = ...
  */
  /* vung moi bat dau tu break hien tai, dai alignedsz byte */
  newrg->vmaid    = vmaid;
  newrg->rg_start = cur_vma->sbrk;
  newrg->rg_end   = cur_vma->sbrk + alignedsz;
  newrg->rg_next  = NULL;

  /* END TODO */

  return newrg;
}

/*validate_overlap_vm_area
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@vmastart: vma end
 *@vmaend: vma end
 *
 * 
 * validate_overlap_vm_area - kiểm khoảng DỰ ĐỊNH [vmastart, vmaend)
 *   0 khi start < end và không chồng VMA nào khác @vmaid (-1 = VMA mới,
 *   so với tất cả); với VMA mới còn phải nằm trong HEAP window. -1 nếu sai.
 *   Chỉ đọc; bên gọi giữ mmvm_lock.
 */
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
    struct vm_area_struct* vma;

    if (caller == NULL || caller->mm == NULL || vmastart >= vmaend)
        return -1;
    if (vmaid < 0 && (vmastart < MM64_UVA_HEAP_START || vmaend > MM64_UVA_HEAP_END))
        return -1;
    for (vma = caller->mm->mmap; vma != NULL; vma = vma->vm_next) {
        if (vmaid >= 0 && vma->vm_id == (unsigned long)vmaid)
            continue;
        if (OVERLAP(vmastart, vmaend, vma->vm_start, vma->vm_end))
            return -1;
    }
    return 0;
}

/*inc_vma_limit - increase vm area limits to reserve space for new variable
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@inc_sz: increment size
 *
 *   inc_vma_limit - SYSMEM_INC_OP: tăng sbrk của VMA heap @vmaid thêm @inc_sz BYTE
 *   new_sbrk = sbrk + inc_sz phải <= vm_end (vm_end cố định, không nới).
 *   Chỉ map các trang MỚI [align_up_page(sbrk), align_up_page(new_sbrk));
 *   vùng mới nằm gọn trong trang đã map thì không map gì.
 *   sbrk chỉ đổi khi map thành công (pt_vm_map nguyên tử). 0 | -1.
 *   Bên gọi giữ mmvm_lock (__alloc giữ; syscall không tự khóa).
 */
int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t inc_sz)
{
    struct vm_area_struct* cur_vma;
    addr_t old_sbrk, new_sbrk, map_start, map_end;

    if (caller == NULL || caller->mm == NULL || inc_sz == 0)
        return -1;
    cur_vma = get_vma_by_num(caller->mm, vmaid);
    if (cur_vma == NULL || !VMA_IS_HEAP(cur_vma->vm_type))
        return -1;

    old_sbrk = cur_vma->sbrk;
    if (inc_sz > cur_vma->vm_end - old_sbrk)
        return -1;                                  
    new_sbrk = old_sbrk + inc_sz;

    if (validate_overlap_vm_area(caller, vmaid, old_sbrk, new_sbrk) < 0)
        return -1;

    map_start = MM64_ALIGN_UP(old_sbrk, PAGING64_PAGESZ);
    map_end = MM64_ALIGN_UP(new_sbrk, PAGING64_PAGESZ);
    if (map_end > map_start &&
        (long)vm_map_ram(caller, cur_vma->vm_start, cur_vma->vm_end, map_start,
            (int)((map_end - map_start) / PAGING64_PAGESZ), NULL) < 0)
        return -1;

    cur_vma->sbrk = new_sbrk;
    return 0;
}

/* =====================================================================
 *   HEAP CLASS, VMA NGANG CẤP, FIRST-FIT CẤP 2
 * ===================================================================== */

enum vm_area_type vm_class_of_size(addr_t size)
{
    if (size <= MM64_HEAP_SMALL_MAX)
        return VMA_HEAP_SMALL;
    if (size <= MM64_HEAP_MEDIUM_MAX)
        return VMA_HEAP_MEDIUM;
    return VMA_HEAP_LARGE;
}

addr_t vm_class_align(enum vm_area_type type)
{
    switch (type) {
    case VMA_HEAP_SMALL:  return MM64_HEAP_ALIGN_SMALL;
    case VMA_HEAP_MEDIUM: return MM64_HEAP_ALIGN_MEDIUM;
    case VMA_HEAP_LARGE:  return PAGING64_PAGESZ;
    default:              return 0;                     
    }
}

/* dung lượng VA của VMA mới cho class; 0 nếu không phải heap / tràn số */
addr_t vm_class_vma_size(enum vm_area_type type, addr_t size)
{
    addr_t len;

    switch (type) {
    case VMA_HEAP_SMALL:  return MM64_VMA_SMALL_SZ;
    case VMA_HEAP_MEDIUM: return MM64_VMA_MEDIUM_SZ;
    case VMA_HEAP_LARGE:
        if (size > (addr_t)-1 - (PAGING64_PAGESZ - 1))
            return 0;
        len = MM64_ALIGN_UP(size, PAGING64_PAGESZ);
        return len > MM64_VMA_LARGE_MIN_SZ ? len : MM64_VMA_LARGE_MIN_SZ;
    default:
        return 0;
    }
}

/*
 * vm_find_heap_gap - First-Fit cấp 2: khoảng VA thấp nhất trong HEAP window
 *   chưa thuộc VMA nào, đủ @len (đã căn trang). Không phụ thuộc thứ tự mmap.
 *   Trả 0 | -1 (hết chỗ / tràn số). Chỉ đọc; bên gọi giữ mmvm_lock.
 */
int vm_find_heap_gap(struct mm_struct* mm, addr_t len, addr_t* out_start)
{
    addr_t cand = MM64_UVA_HEAP_START;
    struct vm_area_struct* v;
    int moved;

    if (mm == NULL || out_start == NULL || len == 0 || (len & (PAGING64_PAGESZ - 1)))
        return -1;
    do {
        if (cand > MM64_UVA_HEAP_END || len > MM64_UVA_HEAP_END - cand)
            return -1;
        moved = 0;
        for (v = mm->mmap; v != NULL; v = v->vm_next) {
            if (OVERLAP(cand, cand + len, v->vm_start, v->vm_end)) {
                cand = MM64_ALIGN_UP(v->vm_end, PAGING64_PAGESZ);
                moved = 1;
                break;
            }
        }
    } while (moved);

    *out_start = cand;
    return 0;
}

/*
 * vm_area_create - tạo VMA heap ngang cấp [start, start+len), sbrk = start,
 *   chưa map trang nào; vm_id = max + 1; nối cuối mmap (VMA0 vẫn đứng đầu).
 *   Bên gọi đã kiểm vùng trống; giữ mmvm_lock. Trả 0 | -1 (không đổi gì).
 */
int vm_area_create(struct mm_struct* mm, enum vm_area_type type,
    addr_t start, addr_t len, unsigned long* out_id)
{
    struct vm_area_struct* v, ** pp, * nv;
    unsigned long id = 0;

    if (mm == NULL || out_id == NULL || !VMA_IS_HEAP(type) || len == 0 ||
        start < MM64_UVA_HEAP_START || start > MM64_UVA_HEAP_END ||
        len > MM64_UVA_HEAP_END - start)
        return -1;
    for (v = mm->mmap; v != NULL; v = v->vm_next) {
        if (OVERLAP(start, start + len, v->vm_start, v->vm_end))
            return -1;
        if (v->vm_id >= id)
            id = v->vm_id + 1;
    }

    nv = malloc(sizeof(*nv));
    if (nv == NULL)
        return -1;
    nv->vm_id = id;
    nv->vm_start = start;
    nv->vm_end = start + len;
    nv->sbrk = start;
    nv->vm_type = type;
    nv->vm_mm = mm;
    nv->vm_freerg_list = NULL;
    nv->vm_next = NULL;

    for (pp = &mm->mmap; *pp != NULL; pp = &(*pp)->vm_next)
        ;
    *pp = nv;
    *out_id = id;
    return 0;
}

/*
 * vm_area_discard - rollback VMA vừa tạo trong cùng giao dịch __alloc:
 *   chỉ khi chưa có PTE (sbrk == vm_start), không region nào trỏ tới,
 *   không phải VMA cố định (id 0..5). Bên gọi giữ mmvm_lock. 0 | -1.
 */
int vm_area_discard(struct mm_struct* mm, unsigned long vm_id)
{
    struct vm_area_struct** pp, * v;
    int i;

    if (mm == NULL || vm_id <= MM64_VMA_ID_RESERVED_MAX)
        return -1;
    for (pp = &mm->mmap; *pp != NULL && (*pp)->vm_id != vm_id; pp = &(*pp)->vm_next)
        ;
    v = *pp;
    if (v == NULL || v->sbrk != v->vm_start || v->vm_freerg_list != NULL)
        return -1;
    for (i = 0; i < PAGING_MAX_SYMTBL_SZ; i++)
        if (mm->symrgtbl[i].active == 1 && mm->symrgtbl[i].vmaid == (int)vm_id)
            return -1;

    *pp = v->vm_next;
    free(v);
    return 0;
}

// #endif
