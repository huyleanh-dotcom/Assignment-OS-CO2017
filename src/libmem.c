/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System Library
 * Memory Module Library libmem.c
 */

// #ifdef MM_PAGING

#include "string.h"
#include "mm64.h"
#include "syscall.h"
#include "libmem.h"
#include "dma.h"
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>

static pthread_mutex_t mmvm_lock = PTHREAD_MUTEX_INITIALIZER;

#if defined(IODUMP) && defined(PAGETBL_DUMP)
/* print_pgtbl chỉ đọc cây nhưng cây có thể đang bị CPU khác sửa -> giữ lock */
static void pgtbl_dump_locked(struct pcb_t* proc)
{
    pthread_mutex_lock(&mmvm_lock);
    print_pgtbl(proc, 0, -1); /* print max TBL */
    pthread_mutex_unlock(&mmvm_lock);
}
#endif
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  struct vm_area_struct* vma;
  struct vm_rg_struct* it;

  if (mm == NULL || mm->mmap == NULL || rg_elmt == NULL)
    return -1;
  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;
  vma = get_vma_by_num(mm, rg_elmt->vmaid);
  if (vma == NULL || rg_elmt->rg_start < vma->vm_start || rg_elmt->rg_end > vma->sbrk)
      return -1;
 
  for (it = vma->vm_freerg_list; it != NULL; it = it->rg_next) {
      if (it->rg_end == rg_elmt->rg_start) { it->rg_end = rg_elmt->rg_end; free(rg_elmt); return 0; }
      if (rg_elmt->rg_end == it->rg_start) { it->rg_start = rg_elmt->rg_start; free(rg_elmt); return 0; }
  }

  /* Enlist the new region */
  rg_elmt->rg_next = vma->vm_freerg_list;
  vma->vm_freerg_list = rg_elmt;

  return 0;
}

/*get_symrg_byid - get mem region by region ID
 *@mm: memory region
 *@rgid: region ID act as symbol index of variable
 *
 */
struct vm_rg_struct *get_symrg_byid(struct mm_struct *mm, int rgid)
{
  /* bien hop le la 0..MAX-1 nen phai '>=' (ban goc dung '>') */
  if (mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    return NULL;

  return &mm->symrgtbl[rgid];
}

/* tra 0 neu truy cap vung rgid tai offset la hop le, nguoc lai -1.
 * kiem: rgid hop le, vung dang active, offset khong vuot kich thuoc vung.
 * READ/WRITE goi truoc khi cham frame de tranh dung nham vung da free. */
int vm_validate_active_region(struct pcb_t *caller, int rgid, addr_t offset)
{
  if (caller == NULL || caller->mm == NULL) return -1;
  struct vm_rg_struct *rg = get_symrg_byid(caller->mm, rgid);
  if (rg == NULL || rg->active != 1) return -1;
  if (rg->rg_start >= rg->rg_end) return -1;
  if (offset >= rg->rg_end - rg->rg_start) return -1;   /* offset vuot vung */
  return 0;
}

/* Validate day du mot lan truy cap [offset, offset+size) trong vung rgid.
 * Kiem: vung active, vmaid khop, offset+size nam gon trong vung (khong tran so),
 * dia chi van thuoc user space (duoi kernel_base). Tra *out_vaddr = VA da xac thuc.
 * P3 goi truoc READ/WRITE/copy de chan truy cap sai. */
extern unsigned long kernel_base;   /* dinh nghia trong os.c, khai bao o common.h */
int vm_validate_active_access(struct pcb_t *caller, int vmaid, int rgid,
                              addr_t offset, addr_t size, addr_t *out_vaddr)
{
  if (caller == NULL || caller->mm == NULL) return -1;
  struct vm_rg_struct *rg = get_symrg_byid(caller->mm, rgid);
  if (rg == NULL || rg->active != 1) return -1;
  if (rg->rg_start >= rg->rg_end) return -1;
  if (rg->vmaid != vmaid) return -1;                 /* sai vung dia chi */
  if (size == 0) return -1;
  addr_t rg_sz = rg->rg_end - rg->rg_start;
  if (offset >= rg_sz) return -1;                    /* offset ngoai vung */
  if (size > rg_sz - offset) return -1;              /* offset+size vuot (khong tran so) */
  if (rg->rg_end > (addr_t)kernel_base) return -1;   /* vung lan sang kernel space */
  if (out_vaddr != NULL) *out_vaddr = rg->rg_start + offset;
  return 0;
}

/* tra 1 neu co vung active nao de len page [page_vaddr, +PAGESZ). */
int vm_page_has_active_region(struct mm_struct *mm, addr_t page_vaddr)
{
  if (mm == NULL) return 0;
  addr_t pg_start = page_vaddr, pg_end = page_vaddr + PAGING64_PAGESZ;
  for (int i = 0; i < PAGING_MAX_SYMTBL_SZ; i++) {
    struct vm_rg_struct *rg = &mm->symrgtbl[i];
    if (rg->active == 1 && rg->rg_start < pg_end && pg_start < rg->rg_end)
      return 1;
  }
  return 0;
}

/* trả [start, start+len) về free list của VMA @vmaid (rollback / free) */
static int vm_give_back(struct mm_struct* mm, int vmaid, addr_t start, addr_t len)
{
    struct vm_rg_struct* rg = malloc(sizeof(*rg));

    if (rg == NULL)
        return -1;
    rg->vmaid = vmaid;
    rg->rg_start = start;
    rg->rg_end = start + len;
    rg->active = 0;
    rg->rg_next = NULL;
    if (enlist_vm_freerg_list(mm, rg) != 0) {
        free(rg);
        return -1;
    }
    return 0;
}

/* syscall 17 SYSMEM_INC_OP: tăng sbrk của VMA @vmaid thêm @inc byte */
static int vm_sys_inc(struct pcb_t* caller, unsigned long vmaid, addr_t inc)
{
    struct sc_regs regs;

    regs.a1 = SYSMEM_INC_OP;
    regs.a2 = vmaid;
    regs.a3 = inc;
    return _syscall(caller->krnl, caller->pid, 17, &regs);
}


/*__alloc - allocate a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *@alloc_addr: address of allocated memory region
 *
 */
int __alloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /*Allocate at the toproof */
  pthread_mutex_lock(&mmvm_lock);

  /* kiem tra tham so */
  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ || size == 0 || caller->mm->symrgtbl[rgid].active == 1)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* chon heap class theo size; tham so vmaid khong con dung */
  (void)vmaid;
  enum vm_area_type cls = vm_class_of_size(size);
  addr_t align = vm_class_align(cls);
  addr_t need = MM64_ALIGN_UP(size, align);

  struct vm_rg_struct rgnode;
  struct vm_area_struct *cur_vma;

  /* tim vung free trong MOI VMA cung class */
  for (cur_vma = caller->mm->mmap; cur_vma != NULL; cur_vma = cur_vma->vm_next)
  {
      if (cur_vma->vm_type == cls &&
          get_free_vmrg_area(caller, (int)cur_vma->vm_id, (int)need, &rgnode) == 0)
          break;
  }


  if (cur_vma != NULL)
  {
      if (vm_zero_allocated_range(caller, 0, rgnode.rg_start, rgnode.rg_start + size) != PT_STATUS_OK)
      {
          vm_give_back(caller->mm, (int)cur_vma->vm_id, rgnode.rg_start, need);
          pthread_mutex_unlock(&mmvm_lock);
          return -1;
      }
      caller->mm->symrgtbl[rgid].vmaid = (int)cur_vma->vm_id;     
      caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
      caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_start + size;  
      caller->mm->symrgtbl[rgid].active = 1;
      *alloc_addr = rgnode.rg_start;
      pthread_mutex_unlock(&mmvm_lock);
      return 0;
  }


  /* TODO get_free_vmrg_area FAILED handle the region management (Fig.6)*/
  /* khong co vung free -> tim VMA cung class con cho tu sbrk toi vm_end */
  for (cur_vma = caller->mm->mmap; cur_vma != NULL; cur_vma = cur_vma->vm_next)
  {
      if (cur_vma->vm_type == cls &&
          MM64_ALIGN_UP(cur_vma->sbrk, align) + size <= cur_vma->vm_end)
          break;
  }

  /* khong VMA nao du cho -> tao VMA ngang cap moi (First-Fit cap 2) */
  int new_vma = 0;
  if (cur_vma == NULL)
  {
      addr_t len = vm_class_vma_size(cls, size);
      addr_t gap;
      unsigned long id;
      if (len == 0 || vm_find_heap_gap(caller->mm, len, &gap) != 0 ||
          vm_area_create(caller->mm, cls, gap, len, &id) != 0)
      {
          pthread_mutex_unlock(&mmvm_lock);
          return -1;
      }
      cur_vma = get_vma_by_num(caller->mm, (int)id);
      new_vma = 1;
  }
  /* khong co vung free vua -> xin them qua syscall; nho break cu */
  addr_t old_sbrk = cur_vma->sbrk;
  addr_t start = MM64_ALIGN_UP(old_sbrk, align);

  /* TODO INCREASE THE LIMIT
   * SYSCALL 1 sys_memmap
   */
  /* goi SYSCALL 17 sys_memmap voi op SYSMEM_INC_OP */
  
  if (vm_sys_inc(caller, cur_vma->vm_id, start + size - old_sbrk) != 0)
  {
      if (new_vma)
          vm_area_discard(caller->mm, cur_vma->vm_id);    
      pthread_mutex_unlock(&mmvm_lock);
      return -1;  
  }

  if (vm_zero_allocated_range(caller, 0, start, start + size) != PT_STATUS_OK)
  {
      vm_give_back(caller->mm, (int)cur_vma->vm_id, start, need);
      pthread_mutex_unlock(&mmvm_lock);
      return -1;
  }

  /*Successful increase limit */
  caller->mm->symrgtbl[rgid].vmaid    = (int)cur_vma->vm_id;
  caller->mm->symrgtbl[rgid].rg_start = start;
  caller->mm->symrgtbl[rgid].rg_end = start + size;
  caller->mm->symrgtbl[rgid].active   = 1;

  *alloc_addr = start;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;

}

/*__free - remove a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __free(struct pcb_t* caller, int vmaid, int rgid)
{
    struct vm_rg_struct* rgnode;
    struct vm_area_struct* vma;
    addr_t end;

    (void)vmaid;
    if (caller == NULL || caller->mm == NULL)
        return -1;
    pthread_mutex_lock(&mmvm_lock);
    if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
    {
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }
    rgnode = get_symrg_byid(caller->mm, rgid);
    if (rgnode == NULL || rgnode->active != 1)
    {
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }
    vma = get_vma_by_num(caller->mm, rgnode->vmaid);
    if (vma == NULL || !VMA_IS_HEAP(vma->vm_type))
    {
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }
    end = MM64_ALIGN_UP(rgnode->rg_end, vm_class_align(vma->vm_type));
    if (end > vma->sbrk)
        end = vma->sbrk;
    if (vm_give_back(caller->mm, rgnode->vmaid, rgnode->rg_start, end - rgnode->rg_start) != 0)
    {
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }

    rgnode->active = 0;                      
    rgnode->rg_start = rgnode->rg_end = 0;
    rgnode->rg_next = NULL;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
}

/*liballoc - PAGING-based allocate a region memory
 *@proc:  Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */
int liballoc(struct pcb_t *proc, addr_t size, uint32_t reg_index)
{
  addr_t  addr;
  int val = __alloc(proc, 0, reg_index, size, &addr);
  if (val != 0)
  {
    return -1;
  }

  /* luu dia chi da cap vao thanh ghi */
  if (reg_index < sizeof(proc->regs) / sizeof(proc->regs[0]))
  {
	  proc->regs[reg_index] = addr;
  }

#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  pgtbl_dump_locked(proc);
#endif
#endif

  /* By default using vmaid = 0 */
  return 0;
}

/*libfree - PAGING-based free a region memory
 *@proc: Process executing the instruction
 *@size: allocated size
 *@reg_index: memory region ID (used to identify variable in symbole table)
 */

int libfree(struct pcb_t *proc, uint32_t reg_index)
{
  int val = __free(proc, 0, reg_index);
  if (val != 0)
  {
    return -1;
  }
  /* bo dong printf debug cua ban goc */
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  pgtbl_dump_locked(proc); // print max TBL
#endif
#endif
  return 0;//val;
}


static struct memphy_struct* pt_swap_dev(struct krnl_t* krnl, int swptyp);

/*
 * pg_swapout - đẩy 1 trang User đang PRESENT của owner ra SWAP
 * @owner     : process sở hữu trang (pte_set_swap thao tác trên owner->mm)
 * @vicpgn    : PGN của trang nạn nhân (đã được gỡ khỏi fifo_pgn)
 * @freed_fpn : frame RAM vừa rảnh — trả thẳng cho người gọi dùng lại,
 *              KHÔNG đưa về free list (tránh CPU khác giành mất)
 * Lỗi: trang vẫn PRESENT, được đưa lại vào fifo_pgn; slot SWAP đã lấy được trả lại.
 * Trả 0 | -1: trang vẫn PRESENT, đã đưa lại vào fifo_pgn, slot SWAP trả lại
 * (thường do SWAP đầy) | -2: nút fifo cũ (trang không còn PRESENT), đã bỏ.
 */
static int pg_swapout(struct pcb_t* owner, addr_t vicpgn, addr_t* freed_fpn)
{
    struct pt_translation t;
    addr_t va = vicpgn << MM64_PAGE_SHIFT;
    addr_t swpfpn;

    if (pt_translate_va(owner, va, PT_USER_READ, &t) != PT_STATUS_OK)
        return -2;                       /* fifo lệch với PTE: không đụng gì */

    if (MEMPHY_get_freefp(owner->krnl->active_mswp, &swpfpn) != 0)
        goto keep;
    if (__mm_swap_page(owner, t.fpn, swpfpn) != 0 ||
        pte_set_swap(owner, vicpgn, (int)owner->krnl->active_mswp_id, swpfpn) != PT_STATUS_OK) {
        MEMPHY_put_freefp(owner->krnl->active_mswp, swpfpn);
        goto keep;
    }
    *freed_fpn = t.fpn;
    return 0;

keep:
    enlist_pgn_node(&owner->mm->fifo_pgn, vicpgn);
    return -1;
}

/* -------- reclaim toàn cục (tầng 1 của alloc_pages_range) ------ */
struct pg_reclaim_ctx {
    struct pcb_t* caller;
    int need, freed;
    int progress;                      /* vòng này có lấy được frame nào không */
    int swap_full;
};

/* lấy 1 trang (cũ nhất) của process p; bỏ qua caller và process lạ */
static int pg_reclaim_from(struct pcb_t* p, void* arg)
{
    struct pg_reclaim_ctx* c = arg;
    addr_t pgn, fpn;

    if (p == c->caller || p->krnl != c->caller->krnl || p->mm == NULL || p->mm->pgd == NULL)
        return 0;
    while (find_victim_page(p->mm, &pgn) == 0) {
        int rc = pg_swapout(p, pgn, &fpn);
        if (rc == -2)
            continue;                      /* nút cũ: thử trang kế của p */
        if (rc != 0) {
            c->swap_full = 1;              /* SWAP đầy: dừng cả lượt duyệt */
            return 1;
        }
        MEMPHY_put_freefp(c->caller->krnl->mram, fpn);
        c->freed++;
        c->progress = 1;
        break;                           /* mỗi process 1 trang / vòng */
    }
    return c->freed >= c->need;
}

/*
 * mm_reclaim_global_user - hook tầng 1 của alloc_pages_range (I3.2)
 * @caller: process đang xin frame (KHÔNG bị lấy trang ở tầng này)
 * @need  : số frame còn thiếu
 *   Duyệt registry theo vòng: mỗi vòng lấy 1 trang cũ nhất (FIFO) của mỗi
 *   process khác, đến khi đủ @need, không ai còn trang, hoặc SWAP đầy.
 *   Chia đều theo vòng để không vét sạch một process duy nhất.
 *   Trả số frame đã trả về RAM free list. Bên gọi giữ mmvm_lock.
 */
int mm_reclaim_global_user(struct pcb_t* caller, int need)
{
    struct pg_reclaim_ctx c = { caller, need, 0, 0, 0 };

    if (caller == NULL || caller->krnl == NULL || need <= 0)
        return 0;
    do {
        c.progress = 0;
        if (registry_foreach_process(caller->krnl, pg_reclaim_from, &c) < 0)
            break;
    } while (c.freed < need && c.progress && !c.swap_full);
    return c.freed;
}

/*
 * mm_swap_active_user - hook tầng 3 của alloc_pages_range
 * @caller: process đang xin frame
 * @need  : số frame còn thiếu
 *   Đẩy tối đa @need trang User cũ nhất (FIFO) của CHÍNH caller ra SWAP,
 *   trả frame về RAM free list để alloc_pages_range lấy lại.
 *   Trả số frame đã trả về (0 = không còn trang để đẩy hoặc SWAP đầy).
 *   Bên gọi giữ mmvm_lock (mọi lần lấy frame đều dưới lock này nên frame
 *   vừa trả không bị CPU khác giành).
 */
int mm_swap_active_user(struct pcb_t* caller, int need)
{
    addr_t pgn, fpn;
    int freed = 0;

    if (caller == NULL || caller->mm == NULL || caller->krnl == NULL)
        return 0;

    while (freed < need && find_victim_page(caller->mm, &pgn) == 0) {
        int rc = pg_swapout(caller, pgn, &fpn);
        if (rc == -2)
            continue;                      
        if (rc != 0)
            break;                         /* SWAP đầy: dừng, trang đã về lại fifo */
        MEMPHY_put_freefp(caller->krnl->mram, fpn);
        freed++;
    }
    return freed;
}


/*
 * pg_get_frame - lấy 1 frame RAM cho trang sắp nạp
 *   Thử free list; hết thì gọi lần lượt 3 tầng reclaim, mỗi tầng xin 1
 *   frame, cùng thứ tự với alloc_pages_range (process khác -> kernel
 *   cache -> trang cũ nhất của chính caller). Trả 0 | -1 (cả 3 tầng cạn).
 *   Bên gọi giữ mmvm_lock.
 */
static int pg_get_frame(struct pcb_t* caller, addr_t* fpn)
{
    int (* const stages[])(struct pcb_t*, int) = {
        mm_reclaim_global_user, mm_reclaim_kernel_cache, mm_swap_active_user
    };
    const int nstages = (int)(sizeof(stages) / sizeof(stages[0]));
    int i;

    for (i = 0; ; i++) {
        if (MEMPHY_get_freefp(caller->krnl->mram, fpn) == 0)
            return 0;
        if (i >= nstages)
            return -1;
        stages[i](caller, 1);          /* kết quả kiểm lại bằng free list */
    }
}

/*pg_getpage - get the page in ram
 *@mm: memory region (luôn là caller->mm; giữ tham số cho tương thích P3)
 *@pgn: PGN
 *@fpn: return FPN
 *@caller: caller
 *
 * PRESENT -> trả fpn. SWAPPED -> lấy frame (RAM trống, nếu hết thì
 * đẩy trang cũ nhất của chính caller ra SWAP), copy SWAP->RAM, commit.
 * EMPTY / INVALID / Kernel -> -1. Bên gọi giữ mmvm_lock.
 */
int pg_getpage(struct mm_struct* mm, addr_t pgn, addr_t* fpn, struct pcb_t* caller)
{
    struct pt_translation t;
    struct memphy_struct* swp;
    addr_t va = pgn << MM64_PAGE_SHIFT;
    addr_t tgtfpn;
    int rc;

    if (caller == NULL || caller->mm == NULL || caller->krnl == NULL || fpn == NULL)
        return -1;
    if (mm != NULL && mm != caller->mm)
        return -1;                       /* chỉ phục vụ address space của caller */

    rc = pt_translate_va(caller, va, PT_USER_READ, &t);
    if (rc == PT_STATUS_OK) {
        *fpn = t.fpn;
        return 0;
    }
    if (rc != PT_STATUS_SWAPPED)
        return -1;                       /* chưa map / không có quyền */

    swp = pt_swap_dev(caller->krnl, t.swptyp);
    if (swp == NULL)
        return -1;

    /* 1. frame đích: RAM trống -> reclaim theo tầng như alloc_pages_range */
    if (pg_get_frame(caller, &tgtfpn) != 0)
        return -1;

    /* 2. SWAP -> RAM, 3. commit PTE (pt_swapin_commit tự enlist fifo_pgn) */
    if (__swap_cp_page(swp, t.swpoff, caller->krnl->mram, tgtfpn) != 0 ||
        pt_swapin_commit(caller, va, tgtfpn) != PT_STATUS_OK) {
        MEMPHY_put_freefp(caller->krnl->mram, tgtfpn);
        return -1;
    }

    /* 4. trả slot SWAP sau khi PTE đã trỏ sang RAM */
    MEMPHY_put_freefp(swp, t.swpoff);
    *fpn = tgtfpn;
    return 0;
}

/* nạp trang chứa addr vào RAM rồi dịch lại với đúng quyền R/W */
static int pg_access_phys(struct pcb_t* caller, addr_t addr, int is_write, addr_t* phys)
{
    struct pt_translation t;
    addr_t fpn;

    if (pg_getpage(caller->mm, addr >> MM64_PAGE_SHIFT, &fpn, caller) != 0)
        return -1;
    if (pt_translate_va(caller, addr, is_write ? PT_USER_WRITE : PT_USER_READ, &t)
        != PT_STATUS_OK)
        return -1;                    
    *phys = pt_translation_phys(&t);
    pt_update_access_flags(caller, addr, is_write);
    return 0;
}

/*pg_getval - read value at given offset
 *@mm: memory region
 *@addr: virtual address to acess (addr_t: VA 57 bit)
 *@value: value
 */
int pg_getval(struct mm_struct* mm, addr_t addr, BYTE* data, struct pcb_t* caller)
{
    addr_t phys;

    if (data == NULL || pg_access_phys(caller, addr, 0, &phys) != 0)
        return -1;                       /* invalid page access */
    (void)mm;
    return MEMPHY_read(caller->krnl->mram, phys, data);
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess (addr_t: VA 57 bit)
 *@value: value
 */
int pg_setval(struct mm_struct* mm, addr_t addr, BYTE value, struct pcb_t* caller)
{
    addr_t phys;

    if (pg_access_phys(caller, addr, 1, &phys) != 0)
        return -1;                       /* invalid page access */
    (void)mm;
    return MEMPHY_write(caller->krnl->mram, phys, value);
}

/* region User rgid + offset -> VA; kiểm region active và offset trong biên */
static int pg_region_va(struct pcb_t* caller, int vmaid, int rgid, addr_t offset, addr_t* va)
{
    struct vm_rg_struct* rg;
    struct vm_area_struct* vma;
    (void)vmaid;
    if (caller == NULL || caller->mm == NULL)
        return -1;
    if (!mm_region_is_active(caller->mm, -1, rgid))
        return -1;
    rg = get_symrg_byid(caller->mm, rgid);
    vma = get_vma_by_num(caller->mm, rg->vmaid);
    if (vma == NULL || !VMA_IS_HEAP(vma->vm_type) || rg->rg_start < vma->vm_start || rg->rg_end > vma->sbrk)
        return -1;
    if (offset >= rg->rg_end - rg->rg_start)
        return -1;                       /* vượt region */
    *va = rg->rg_start + offset;
    return 0;
}

/*__read - read value in region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 */
int __read(struct pcb_t* caller, int vmaid, int rgid, addr_t offset, BYTE* data)
{
    addr_t va;
    int rc = -1;

    pthread_mutex_lock(&mmvm_lock);
    if (pg_region_va(caller, vmaid, rgid, offset, &va) == 0)
        rc = pg_getval(caller->mm, va, data, caller);
    pthread_mutex_unlock(&mmvm_lock);
    return rc == 0 ? 0 : -1;
}

/*libread - PAGING-based read a region memory */
int libread(
    struct pcb_t* proc, // Process executing the instruction
    uint32_t source,    // Index of source register
    addr_t offset,    // Source address = [source] + [offset]
    uint32_t* destination)
{
    BYTE data;
    int val = __read(proc, 0, source, offset, &data);

    if (val != 0)
        return -1;
    *destination = data;
#ifdef IODUMP
    /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
    pgtbl_dump_locked(proc); 
#endif
#endif

    return 0;
}

/*__write - write a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 */
int __write(struct pcb_t* caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
    addr_t va;
    int rc = -1;

    pthread_mutex_lock(&mmvm_lock);
    if (pg_region_va(caller, vmaid, rgid, offset, &va) == 0)
        rc = pg_setval(caller->mm, va, value, caller);
    pthread_mutex_unlock(&mmvm_lock);
    return rc == 0 ? 0 : -1;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t* proc,   // Process executing the instruction
    BYTE data,            // Data to be wrttien into memory
    uint32_t destination, // Index of destination register
    addr_t offset)
{
    int val = __write(proc, 0, destination, offset, data);
    if (val != 0)
    {
        return -1;
    }
#ifdef IODUMP
    /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
    pgtbl_dump_locked(proc); // print max TBL
#endif
#endif

    return 0;
}


/*libkmem_malloc- alloc region memory in kmem
 *@caller: caller
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 */

/* Kernel virtual addresses use krnl->kva_brk and krnl->kva_free. */
#define KVA_TOP ((addr_t)0xFFFFFFFFFFFFF000UL)        /* trang cuối không dùng */

/* gỡ npg trang Kernel từ start, KHÔNG trả frame (bên gọi tự trả). Trả số trang đã gỡ */
static int kmem_unmap_pages(struct pcb_t* caller, addr_t start, addr_t npg)
{
    addr_t i;
    int n = 0;

    for (i = 0; i < npg; i++) {
        struct pt_mapping_info old;
        if (pt_unmap_kernel_one(caller, start + i * PAGING64_PAGESZ, &old) == PT_STATUS_OK)
            n++;
    }
    return n;
}

/* Best-Fit: khoảng trống nhỏ nhất >= len (bằng nhau lấy địa chỉ thấp); không có thì
 * nới brk. Bên gọi giữ kmm_lock. 0 | -1 (không đổi gì). */
static int kva_alloc_locked(struct krnl_t* krnl, addr_t len, addr_t* start)
{
    struct kva_range** pp, ** best = NULL;

    if (krnl == NULL || start == NULL || len == 0 || (len & (PAGING64_PAGESZ - 1)))
        return -1;
    if (krnl->kva_brk == 0)
        krnl->kva_brk = (addr_t)kernel_base;

    for (pp = &krnl->kva_free; *pp != NULL; pp = &(*pp)->next) {
        addr_t sz = (*pp)->end - (*pp)->start;
        if (sz >= len && (best == NULL || sz < (*best)->end - (*best)->start))
            best = pp;
    }
    if (best != NULL) {
        struct kva_range* r = *best;
        *start = r->start;
        if (r->end - r->start == len) {       
            *best = r->next;
            free(r);
        }
        else
            r->start += len;                  
        return 0;
    }

    if (krnl->kva_brk > KVA_TOP || len > KVA_TOP - krnl->kva_brk)
        return -1;                             
    *start = krnl->kva_brk;
    krnl->kva_brk += len;
    return 0;
}
/* trả [start, start+len) đã unmap; gộp trước/sau; chạm brk thì hạ brk.
 * Bên gọi giữ kmm_lock. -1 (không đổi gì): lệch trang, ngoài [base, brk),
 * chồng khoảng trống (trả trùng), hết bộ nhớ Host. */
static int kva_free_locked(struct krnl_t* krnl, addr_t start, addr_t len)
{
    struct kva_range** pp, * prev = NULL, * next, * nr;
    addr_t end;

    if (krnl == NULL || len == 0 || ((start | len) & (PAGING64_PAGESZ - 1)) ||
        start < (addr_t)kernel_base || start > krnl->kva_brk || len > krnl->kva_brk - start)
        return -1;
    end = start + len;

    for (pp = &krnl->kva_free; *pp != NULL && (*pp)->start < start; pp = &(*pp)->next)
        prev = *pp;
    next = *pp;
    if ((prev != NULL && prev->end > start) || (next != NULL && next->start < end))
        return -1;                              /* chồng khoảng trống: trả trùng */

	if (prev != NULL && prev->end == start) {   //merge with previous range
        prev->end = end;
        if (next != NULL && next->start == end) {
            prev->end = next->end;
            prev->next = next->next;
            free(next);
        }
        nr = prev;
    }
	else if (next != NULL && next->start == end) {  //merge with next range
        next->start = start;
        nr = next;
    }
    else {
        nr = malloc(sizeof(*nr));
        if (nr == NULL)
            return -1;
        nr->start = start;
        nr->end = end;
        nr->next = next;
        *pp = nr;
    }

    if (nr->end == krnl->kva_brk && nr->next == NULL) {   /* chạm đỉnh: hạ brk */
        struct kva_range** q;
        krnl->kva_brk = nr->start;
        for (q = &krnl->kva_free; *q != nr; q = &(*q)->next)
            ;
        *q = NULL;
        free(nr);
    }
    return 0;
}

int kva_alloc(struct krnl_t* krnl, addr_t len, addr_t* start)
{
    int rc;
    pthread_mutex_lock(&kmm_lock);
    rc = kva_alloc_locked(krnl, len, start);
    pthread_mutex_unlock(&kmm_lock);
    return rc;
}

int kva_free(struct krnl_t* krnl, addr_t start, addr_t len)
{
    int rc;
    pthread_mutex_lock(&kmm_lock);
    rc = kva_free_locked(krnl, start, len);
    pthread_mutex_unlock(&kmm_lock);
    return rc;
}


#define KMEM_BIT_TEST(c, i) ((c)->used[(i) >> 3] &   (1u << ((i) & 7)))
#define KMEM_BIT_SET(c, i)  ((c)->used[(i) >> 3] |=  (1u << ((i) & 7)))
#define KMEM_BIT_CLR(c, i)  ((c)->used[(i) >> 3] &= ~(1u << ((i) & 7)))

 
int kmem_pool_index(addr_t size)
{
    static const int sizes[KMEM_NUM_POOLS] = { 16, 32, 64, 96, 128, 192, 256, 512, 1024, 2048 };
    int i;

    if (size == 0 || size > KMEM_MAX_SLOT)
        return -1;
    for (i = 0; i < KMEM_NUM_POOLS; i++)
        if (size <= (addr_t)sizes[i])
            return i;
    return -1;
}

/*
 * kmem_pool_grow - thêm 1 chunk (1 trang Kernel có frame) vào pool
 */
int kmem_pool_grow(struct pcb_t* caller, struct kcache_pool_struct* pool)
{
    struct kmem_chunk* c;
    addr_t base;
    int i, n;

    if (caller == NULL || caller->krnl == NULL || pool == NULL || pool->size <= 0)
        return -1;
    n = pool->slots_per_chunk;
    if (n <= 0 || n > KMEM_CHUNK_SLOTS)
        return -1;

    c = calloc(1, sizeof(*c));
    if (c == NULL)
        return -1;
    if (kva_alloc(caller->krnl, KMEM_CHUNK_SIZE, &base) != 0) {
        free(c);
        return -1;
    }
    if ((long)vm_map_kernel(caller, base, base + KMEM_CHUNK_SIZE, base, 1, NULL) < 0) {
        kva_free(caller->krnl, base, KMEM_CHUNK_SIZE);
        free(c);
        return -1;
    }

    c->base = base;
    c->nfree = n;
    c->free_head = 0;
    for (i = 0; i < n; i++)
        c->next_free[i] = (short)(i + 1 < n ? i + 1 : -1);
    c->next = pool->chunks;
    pool->chunks = c;
    pool->nchunks++;
    pool->storage = base;
    return 0;
}

/*
 * kmem_pool_alloc_slot - lấy 1 slot, xóa 0; grow nếu mọi chunk đã đầy.
 *   Chọn chunk còn chỗ nhưng ÍT slot trống nhất: dồn object vào chunk gần
 *   đầy để chunk khác có cơ hội rỗng hẳn và được reclaim.
 *   Trả kernel VA | (addr_t)-1. Bên gọi giữ mmvm_lock.
 */
addr_t kmem_pool_alloc_slot(struct pcb_t* caller, struct kcache_pool_struct* pool)
{
    struct kmem_chunk* c = NULL, * it;
    addr_t kva;
    int i;

    if (caller == NULL || pool == NULL || pool->size <= 0)
        return (addr_t)-1;
    for (it = pool->chunks; it != NULL; it = it->next)
        if (it->nfree > 0 && (c == NULL || it->nfree < c->nfree))
            c = it;
    if (c == NULL) {
        if (kmem_pool_grow(caller, pool) != 0)
            return (addr_t)-1;
        c = pool->chunks;
    }

    i = c->free_head;
    c->free_head = c->next_free[i];
    c->nfree--;
    KMEM_BIT_SET(c, i);

    kva = c->base + (addr_t)i * (addr_t)pool->size;
    vm_zero_allocated_range(caller, 1, kva, kva + (addr_t)pool->size);
    return kva;
}

/*
 * kmem_pool_free_slot - trả 1 slot; chặn lệch biên slot, phần thừa cuối
 *   chunk, địa chỉ lạ và trả trùng (bitmap). 0 | -1 (không đổi gì).
 *   Bên gọi giữ mmvm_lock.
 */
int kmem_pool_free_slot(struct kcache_pool_struct* pool, addr_t kva)
{
    struct kmem_chunk* c;

    if (pool == NULL || pool->size <= 0)
        return -1;
    for (c = pool->chunks; c != NULL; c = c->next) {
        addr_t off;
        int i;

        if (kva < c->base || kva >= c->base + KMEM_CHUNK_SIZE)
            continue;
        off = kva - c->base;
        i = (int)(off / (addr_t)pool->size);
        if (off % (addr_t)pool->size != 0 || i >= pool->slots_per_chunk || !KMEM_BIT_TEST(c, i))
            return -1;
        KMEM_BIT_CLR(c, i);
        c->next_free[i] = (short)c->free_head;
        c->free_head = i;
        c->nfree++;
        return 0;
    }
    return -1;
}

/* trả 1 chunk đã gỡ khỏi pool: unmap -> trả frame -> trả VA. Trả 1 nếu trả được frame */
static int kmem_chunk_release(struct pcb_t* caller, struct kmem_chunk* c)
{
    struct pt_mapping_info old;
    int got = 0;

    if (pt_unmap_kernel_one(caller, c->base, &old) == PT_STATUS_OK && old.state == PT_MAP_PRESENT)
        got = (MEMPHY_put_freefp(caller->krnl->mram, old.location.fpn) == 0);
    kva_free(caller->krnl, c->base, KMEM_CHUNK_SIZE);
    free(c);
    return got;
}

/*
 * mm_reclaim_kernel_cache - hook tầng 2 của alloc_pages_range (bản thật)
 *   Lượt 1: trả chunk RỖNG HẲN nhưng không trả chunk cuối cùng của class
 *           (cố giữ lại 1 chunk/class).
 *   Lượt 2: vẫn thiếu thì trả nốt các chunk đã chừa.
 *   Chunk còn slot đang dùng không bao giờ bị đụng. Trả số frame đã trả.
 *   Bên gọi giữ mmvm_lock.
 */
int mm_reclaim_kernel_cache(struct pcb_t* caller, int need)
{
    int pass, p, freed = 0;

    if (caller == NULL || caller->krnl == NULL || caller->krnl->kcpooltbl == NULL || need <= 0)
        return 0;
    for (pass = 1; pass <= 2 && freed < need; pass++) {
        for (p = 0; p < KMEM_NUM_POOLS && freed < need; p++) {
            struct kcache_pool_struct* pool = &caller->krnl->kcpooltbl[p];
            struct kmem_chunk** pp = &pool->chunks;

            while (*pp != NULL && freed < need) {
                struct kmem_chunk* c = *pp;
                if (c->nfree != pool->slots_per_chunk ||      
                    (pass == 1 && pool->nchunks <= 1)) {        
                    pp = &c->next;
                    continue;
                }
                *pp = c->next;                     
                pool->nchunks--;
                freed += kmem_chunk_release(caller, c);
            }
        }
    }
    return freed;
}



/* -------- vùng Kernel theo (pid, reg) — bên gọi giữ mmvm_lock -------- */
struct kmem_region* kregion_lookup(struct krnl_t* krnl, uint32_t pid, int reg)
{
    struct kmem_region* r;

    if (krnl == NULL)
        return NULL;
    for (r = krnl->kregions; r != NULL; r = r->next)
        if (r->pid == pid && r->reg == reg)
            return r;
    return NULL;
}

static void kregion_unlink(struct krnl_t* krnl, struct kmem_region* rec)
{
    struct kmem_region** pp;

    for (pp = &krnl->kregions; *pp != NULL; pp = &(*pp)->next)
        if (*pp == rec) {
            *pp = rec->next;
            return;
        }
}

/*
 * kmem_direct_alloc_locked - dải FPN liên tục n trang + VA Best-Fit + map + xóa 0
 *   Thiếu dải: reclaim 3 tầng (need = n) rồi thử lại ĐÚNG 1 lần; không compaction.
 *   Lỗi: trả mọi thứ đã lấy, -1. Bên gọi giữ mmvm_lock.
 */
static int kmem_direct_alloc_locked(struct pcb_t* caller, addr_t size,
    addr_t* kva, addr_t* first, addr_t* npages)
{
    struct memphy_struct* mram = caller->krnl->mram;
    addr_t n = PAGING64_PAGE_ALIGNSZ(size) / PAGING64_PAGESZ;
    addr_t f, va;

    if (MEMPHY_get_contig_fp(mram, n, &f) != 0) {
        mm_reclaim_global_user(caller, (int)n);
        mm_reclaim_kernel_cache(caller, (int)n);
        mm_swap_active_user(caller, (int)n);
        if (MEMPHY_get_contig_fp(mram, n, &f) != 0)
            return -1;                          /* đủ tổng nhưng không đủ dải */
    }
    if (kva_alloc(caller->krnl, n * PAGING64_PAGESZ, &va) != 0) {
        MEMPHY_put_contig_fp(mram, f, n);
        return -1;
    }
    if (vm_map_kernel_contig(caller, va, f, n) != PT_STATUS_OK) {
        kva_free(caller->krnl, va, n * PAGING64_PAGESZ);
        MEMPHY_put_contig_fp(mram, f, n);
        return -1;
    }
    if (vm_zero_allocated_range(caller, 1, va, va + size) != PT_STATUS_OK) {
        kmem_unmap_pages(caller, va, n);
        MEMPHY_put_contig_fp(mram, f, n);
        kva_free(caller->krnl, va, n * PAGING64_PAGESZ);
        return -1;
    }
    *kva = va;
    *first = f;
    *npages = n;
    return 0;
}

/* giải phóng 1 vùng (đã gỡ khỏi danh sách). Bên gọi giữ mmvm_lock. */
static int kmem_free_region_locked(struct pcb_t* caller, struct kmem_region* rec)
{
    int rc = 0;

    if (rec->kind == KMEM_DIRECT) {
        kmem_unmap_pages(caller, rec->kva, rec->npages);
        if (MEMPHY_put_contig_fp(caller->krnl->mram, rec->first_fpn, rec->npages) != 0)
            rc = -1;
        if (kva_free(caller->krnl, rec->kva, rec->npages * PAGING64_PAGESZ) != 0)
            rc = -1;
    }
    else if (rec->kind == KMEM_POOL) {
        if (caller->krnl->kcpooltbl == NULL ||
            kmem_pool_free_slot(&caller->krnl->kcpooltbl[rec->pool_id], rec->kva) != 0)
            rc = -1;
    }
    free(rec);
    return rc;
}

int __kfree(struct pcb_t* caller, int reg)
{
    struct kmem_region* rec;
    int rc;

    if (caller == NULL || caller->krnl == NULL)
        return -1;
    pthread_mutex_lock(&mmvm_lock);
    rec = kregion_lookup(caller->krnl, caller->pid, reg);
    if (rec == NULL) {
        pthread_mutex_unlock(&mmvm_lock);
        return -1;
    }
    kregion_unlink(caller->krnl, rec);          /* gỡ bản ghi TRƯỚC */
    rc = kmem_free_region_locked(caller, rec);
    pthread_mutex_unlock(&mmvm_lock);
    return rc;
}

/*
 *   mm_iterate_user_mappings - bản có khóa của
 *   pt_iterate_registered_address_spaces cho code NGOÀI libmem.c (DMA, test):
 *   mmvm_lock là static trong libmem.c nên bên ngoài không tự giữ được.
 *   Lấy mmvm_lock -> duyệt -> nhả. cb chịu cùng ràng buộc (chỉ đọc, không
 *   gọi hàm nào lấy mmvm_lock: __alloc/__free/__read/__write/kmalloc...).
 *   Trả như pt_iterate_registered_address_spaces.
 */
int mm_iterate_user_mappings(struct krnl_t* krnl, pt_global_mapping_callback cb, void* ctx)
{
    int rc;

    pthread_mutex_lock(&mmvm_lock);
    rc = pt_iterate_registered_address_spaces(krnl, cb, ctx);
    pthread_mutex_unlock(&mmvm_lock);
    return rc;
}


/* kmem_release_process - trả mọi vùng Kernel của proc (gọi khi process kết thúc,
 * sau registry_unregister, trước free_pcb_memph). Trả số vùng đã trả. */
int kmem_release_process(struct pcb_t* proc)
{
    struct kmem_region** pp;
    int n = 0;

    if (proc == NULL || proc->krnl == NULL)
        return 0;
    pthread_mutex_lock(&mmvm_lock);
    pp = &proc->krnl->kregions;
    while (*pp != NULL) {
        struct kmem_region* rec = *pp;
        if (rec->pid != proc->pid) {
            pp = &rec->next;
            continue;
        }
        *pp = rec->next;
        kmem_free_region_locked(proc, rec);
        n++;
    }
    pthread_mutex_unlock(&mmvm_lock);
    return n;
}

/* kmem_region_phys - địa chỉ vật lý của byte off trong vùng (pid, reg) — cho DMA.
 * Chỉ đọc; bên gọi giữ mmvm_lock nếu cần kết quả ổn định. 0 | -1. */
int kmem_region_phys(struct krnl_t* krnl, uint32_t pid, int reg, addr_t off, addr_t* phys)
{
    struct kmem_region* rec = kregion_lookup(krnl, pid, reg);

    if (rec == NULL || phys == NULL || off >= rec->size)
        return -1;
    if (rec->kind == KMEM_DIRECT) {
        addr_t o = (rec->kva & (PAGING64_PAGESZ - 1)) + off;
        *phys = rec->first_fpn * PAGING64_PAGESZ + o;   /* dải liên tục */
        return 0;
    }
    {   /* POOL: slot nằm gọn trong 1 frame -> tra PTE */
        addr_t pte = pte_get_entry(krnl->mm, mm64_va_to_pgn(rec->kva + off), NULL);
        if (pte_decode_state((mm64_pte_t)pte) != PT_MAP_PRESENT)
            return -1;
        *phys = pte_decode_fpn((mm64_pte_t)pte) * PAGING64_PAGESZ + ((rec->kva + off) & (PAGING64_PAGESZ - 1));
        return 0;
    }
}


int libkmem_malloc(struct pcb_t * caller, uint32_t size, uint32_t reg_index)
{
  /*
   * TODO: provide OS level management
   *       and forward the request to helper
   */
  /* goi __kmalloc roi luu VA kernel vao thanh ghi */
  addr_t addr;
  if (__kmalloc(caller, 0, reg_index, size, &addr) != 0)
    return -1;
  if (reg_index < sizeof(caller->regs) / sizeof(caller->regs[0]))
  {
	  caller->regs[reg_index] = addr;
  }

  /*
   * TODO: provide OS kmem allocation validation
   */
#ifdef IODUMP
#ifdef PAGETBL_DUMP
  pgtbl_dump_locked(caller);
#endif
#endif
  return 0;
}


/*kmalloc - alloc region memory in kmem
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 *@alloc_addr: allocated address
 */
addr_t __kmalloc(struct pcb_t *caller, int vmaid, int rgid, addr_t size, addr_t *alloc_addr)
{
  /* TODO: provide OS kernel memory allocation
   *       update krnl_pgd for OS kernel level management */
  /* cap phat lien tuc tren kernel heap, lay dia chi tu kmem_bump (dung chung
   * break voi slab pool nen khong bi cap de len nhau) */
  
  struct kmem_region* rec;
  addr_t kva, first = 0, npg = 0;
  int cls;

  (void)vmaid;
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL || alloc_addr == NULL)
      return (addr_t)-1;
  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ || size == 0)
      return (addr_t)-1;
  cls = (caller->krnl->kcpooltbl != NULL) ? kmem_pool_index(size) : -1;
  pthread_mutex_lock(&mmvm_lock);
  if (kregion_lookup(caller->krnl, caller->pid, rgid) != NULL)   
  {
      pthread_mutex_unlock(&mmvm_lock);
      return (addr_t)-1;
  }

  rec = malloc(sizeof(*rec));
  if (rec == NULL)
  {
      free(rec);
      pthread_mutex_unlock(&mmvm_lock);
      return (addr_t)-1;
  }
  if (cls >= 0)                                                    
      kva = kmem_pool_alloc_slot(caller, &caller->krnl->kcpooltbl[cls]);
  else if (kmem_direct_alloc_locked(caller, size, &kva, &first, &npg) != 0)
      kva = (addr_t)-1;
  if (kva == (addr_t)-1)
  {
      free(rec);
      pthread_mutex_unlock(&mmvm_lock);
      return (addr_t)-1;
  }
  /* ghi vung vao bang symbol cua kernel mm */
  rec->pid = caller->pid;
  rec->reg = rgid;
  rec->kind = (cls >= 0) ? KMEM_POOL : KMEM_DIRECT;
  rec->pool_id = cls;
  rec->kva = kva;
  rec->size = size;
  rec->first_fpn = first;
  rec->npages = npg;
  rec->next = caller->krnl->kregions;
  caller->krnl->kregions = rec;
  *alloc_addr = kva;

  pthread_mutex_unlock(&mmvm_lock);
  return 0;

}

addr_t __kmem_cache_alloc(struct pcb_t *caller, int vmaid, int rgid, int cache_pool_id, addr_t *alloc_addr)
{
    /* lay 1 slot san co trong pool, het slot thi pool tu xin them tu kernel heap.
     * do slot co kich thuoc co dinh nen it bi phan manh hon goi __kmalloc truc tiep
     * [M4] vung ghi vao bang (pid, reg); frame that cho pool: dot 2 */
    struct kcache_pool_struct* pool;
    struct kmem_region* rec;
    addr_t kva;

    (void)vmaid;
    if (caller == NULL || caller->krnl == NULL || caller->krnl->kcpooltbl == NULL)
        return (addr_t)-1;
    if (cache_pool_id < 0 || cache_pool_id >= KMEM_NUM_POOLS || 
        rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
        return (addr_t)-1;
    pool = &caller->krnl->kcpooltbl[cache_pool_id];
    if (pool->size <= 0)
        return (addr_t)-1;

    pthread_mutex_lock(&mmvm_lock);
    if (kregion_lookup(caller->krnl, caller->pid, rgid) != NULL ||
        (rec = malloc(sizeof(*rec))) == NULL)
    {
        pthread_mutex_unlock(&mmvm_lock);
        return (addr_t)-1;
    }
    kva = kmem_pool_alloc_slot(caller, pool);
    if (kva == (addr_t)-1)
    {
        free(rec);
        pthread_mutex_unlock(&mmvm_lock);
        return (addr_t)-1;
    }
    rec->pid = caller->pid;
    rec->reg = rgid;
    rec->kind = KMEM_POOL;
    rec->pool_id = cache_pool_id;
    rec->kva = kva;
    rec->size = (addr_t)pool->size;
    rec->first_fpn = 0;
    rec->npages = 0;
    rec->next = caller->krnl->kregions;
    caller->krnl->kregions = rec;
    pthread_mutex_unlock(&mmvm_lock);

    if (alloc_addr != NULL)
        *alloc_addr = kva;
    return 0;
}


static struct memphy_struct* pt_swap_dev(struct krnl_t* krnl, int swptyp);
static int pt_region_span(struct mm_struct* mm, int rgid, addr_t offset,
    addr_t size, addr_t* va);
static int pt_user_byte_phys(struct pcb_t* caller, addr_t va, int is_write, addr_t* phys);
static int pt_kmem_region_va(struct pcb_t* caller, int vmaid, int rgid,
    addr_t offset, addr_t* va);
// help function to determine physical address from calling dma in continous memory region 
static int dma_kregion_contig_phys(struct krnl_t* krnl, uint32_t pid, int reg, uint32_t offset, uint32_t length, addr_t* out_phys )
{
    addr_t first_phys =0 ; 
    addr_t i; 
    if(out_phys == NULL || length ==0) return -1;
    if(kmem_region_phys(krnl,pid,reg, offset,&first_phys) !=0 ) return -1;
    for(i=0; i< length; i++){
        addr_t curr_phys =0;
        if(kmem_region_phys(krnl,pid,reg,offset + i,&curr_phys)!= 0) return -1;
        if(curr_phys != first_phys + i) return -1;
    }
    *out_phys = first_phys;
    return 0;
}
static int dma_uregion_contig_phys(struct pcb_t* caller, int rgid, addr_t offset, addr_t length,int is_write, addr_t* out_phys ){
    addr_t va =0, first_phys =0;
    addr_t i;
    if(out_phys == NULL || length ==0) return -1;
    if(pt_region_span(caller->mm, rgid, offset,length,&va) != PT_STATUS_OK) return -1; 
    if (pt_user_byte_phys(caller, va, is_write, &first_phys) != PT_STATUS_OK) return -1;
    for (i = 1; i < length; i++) {
        addr_t cur_phys = 0;
        if (pt_user_byte_phys(caller, va + i, is_write, &cur_phys) != PT_STATUS_OK)
            return -1;
        if (cur_phys != first_phys + i)
            return -1;
    }
    *out_phys = first_phys;
    return 0;
}
/* dma_print_address - show how one DMA operand is seen by the CPU and the DMA.
 * The CPU uses the virtual address, translated by the MMU (page table).
 * The DMA uses the bus address. The system bus maps RAM one to one,
 * so the bus address is the physical address (offset in RAM). */
static void dma_print_address(struct pcb_t *caller, const char *role, uint32_t rgid,
                              int is_kernel, addr_t offset, addr_t phys)
{
    addr_t va = 0;
    int rc;

    if (is_kernel) 
	{
        rc = pt_kmem_region_va(caller, -1, (int)rgid, offset, &va);
    }
	else 
	{
        rc = pt_region_span(caller->mm, (int)rgid, offset, 1, &va);
    }
    if (rc != PT_STATUS_OK) 
	{
        return;
    }
    printf("\tPID %d: dma_memcpy %s = %s region %u: VA 0x%llx -> MMU -> PA 0x%llx = bus address 0x%llx\n",
           caller->pid, role, is_kernel ? "kernel" : "user", rgid,
           (unsigned long long)va, (unsigned long long)phys,
           (unsigned long long)phys);
}
/*libmem_dma_memcpy - DMA memcpy
 *@caller: caller
 *@src: source region
 *@dst: destination region
 *@offset: memory offset
 *@size: copied data size
 */

int libmem_dma_memcpy(struct pcb_t *caller, uint32_t src, uint32_t dst, uint32_t offset, uint32_t size)
{
  /* TODO: provide DMA mempy */

  //struct krnl_t *krnl = caller->krnl;

  /*
   *  TODO: provide asynchronous dma memcpy
   *        handling the dma_done_callback
   */
  //do_dma(caller, (uint64_t) src, (uint64_t) dst, (uint64_t) size);
  //krnl->...
  //krnl->...
    addr_t src_phys = 0, dst_phys = 0;
    int got_src = 0, got_dst = 0;
	int src_kernel = 0, dst_kernel = 0;
    if (size == 0)
        return 0;
    if (caller == NULL || caller->krnl == NULL || caller->krnl->mram == NULL)
        return -1;
    pthread_mutex_lock(&mmvm_lock);
    if (dma_kregion_contig_phys(caller->krnl, caller->pid, (int)src,
                                (addr_t)offset, (addr_t)size, &src_phys) == 0) {
        got_src = 1;
		src_kernel = 1;
    } else if (caller->mm != NULL &&
               dma_uregion_contig_phys(caller, (int)src,
                                       (addr_t)offset, (addr_t)size, 0, &src_phys) == 0) {
        got_src = 1;
    }
    if (dma_kregion_contig_phys(caller->krnl, caller->pid, (int)dst,
                                (addr_t)offset, (addr_t)size, &dst_phys) == 0) {
        got_dst = 1;
		dst_kernel = 1;
    } else if (caller->mm != NULL &&
               dma_uregion_contig_phys(caller, (int)dst,
                                       (addr_t)offset, (addr_t)size, 1, &dst_phys) == 0) {
        got_dst = 1;
    }
	if (got_src && got_dst) 
	{
		dma_print_address(caller, "src", src, src_kernel, (addr_t)offset, src_phys);
		dma_print_address(caller, "dst", dst, dst_kernel, (addr_t)offset, dst_phys);
	}
    pthread_mutex_unlock(&mmvm_lock);
    if (!got_src || !got_dst)
        return -1;
    return do_dma(caller, (uint64_t)src_phys, (uint64_t)dst_phys, (uint64_t)size);
}



__attribute__((weak)) int mm_region_is_active(struct mm_struct* mm, int vmaid, int rgid)
{
    (void)vmaid;
    if (mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
        return 0;
    return mm->symrgtbl[rgid].active == 1 &&                     
        mm->symrgtbl[rgid].rg_start < mm->symrgtbl[rgid].rg_end;
}

/* region Kernel + offset -> VA, có kiểm biên. Trả PT_STATUS_xxx. */
static int pt_kmem_region_va(struct pcb_t* caller, int vmaid, int rgid,
    addr_t offset, addr_t* va)
{
    struct kmem_region* rec;

    (void)vmaid;
    if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL)
        return PT_STATUS_ERROR;
    if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
        return PT_STATUS_ERROR;

    rec = kregion_lookup(caller->krnl, caller->pid, rgid);     
    if (rec == NULL)
        return PT_STATUS_DENIED;
    if (offset >= rec->size)
        return PT_STATUS_DENIED;


    *va = rec->kva + offset;
    return PT_STATUS_OK;
}

/* đọc / ghi 1 byte tại VA Kernel. Bên gọi giữ mmvm_lock. */
static int pt_kmem_access_va_locked(struct pcb_t* caller, addr_t va,
    int is_write, BYTE* data)
{
    struct pt_translation t;
    int rc;

    rc = pt_translate_va(caller, va, is_write ? PT_KERNEL_WRITE : PT_KERNEL_READ, &t);
    if (rc == PT_STATUS_SWAPPED)
        return PT_STATUS_ERROR;                  
    if (rc != PT_STATUS_OK)
        return rc;

    if (is_write)
        rc = MEMPHY_write(caller->krnl->mram, pt_translation_phys(&t), *data);
    else
        rc = MEMPHY_read(caller->krnl->mram, pt_translation_phys(&t), data);
    return (rc == 0) ? PT_STATUS_OK : PT_STATUS_ERROR;
}

static int pt_kmem_read_locked(struct pcb_t* caller, int vmaid, int rgid,
    addr_t offset, BYTE* data)
{
    addr_t va;
    int rc;

    if (data == NULL)
        return PT_STATUS_ERROR;
    rc = pt_kmem_region_va(caller, vmaid, rgid, offset, &va);
    if (rc != PT_STATUS_OK)
        return rc;
    return pt_kmem_access_va_locked(caller, va, 0, data);
}

static int pt_kmem_write_locked(struct pcb_t* caller, int vmaid, int rgid,
    addr_t offset, BYTE value)
{
    addr_t va;
    int rc;

    rc = pt_kmem_region_va(caller, vmaid, rgid, offset, &va);
    if (rc != PT_STATUS_OK)
        return rc;
    return pt_kmem_access_va_locked(caller, va, 1, &value);
}



/*__read_kernel_mem - read value in kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy

    int rc;

    pthread_mutex_lock(&mmvm_lock);
    rc = pt_kmem_read_locked(caller, vmaid, rgid, offset, data);
    pthread_mutex_unlock(&mmvm_lock);
    return rc;
}

/*__write_kernel_mem - write a kernel region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_kernel_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS memory operator for kernel memory region */
  //krnl->krnl_pgd ... or krnl->pgd ... based on kmem implementation strategy

    int rc;

    pthread_mutex_lock(&mmvm_lock);
    rc = pt_kmem_write_locked(caller, vmaid, rgid, offset, value);
    pthread_mutex_unlock(&mmvm_lock);
    return rc;
}

/* P1 copy_from/to_user gặp trang SWAPPED -> nạp lại bằng pg_getpage.
 * bằng bản giả của chúng. Bên gọi giữ mmvm_lock. */
    __attribute__((weak)) int mm_handle_swapped_page(struct pcb_t* caller, addr_t va)
{
    addr_t fpn;
    if (caller == NULL)
        return -1;
    return pg_getpage(caller->mm, va >> MM64_PAGE_SHIFT, &fpn, caller);
}
/* [offset, offset+size) phải nằm trong region rgid (vmaid 0) của mm */
static int pt_region_span(struct mm_struct* mm, int rgid, addr_t offset,
    addr_t size, addr_t* va)
{
    struct vm_rg_struct* rg;
    addr_t len;

    if (mm == NULL || rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)
        return PT_STATUS_ERROR;
    if (!mm_region_is_active(mm, 0, rgid))
        return PT_STATUS_DENIED;
    rg = &mm->symrgtbl[rgid];
    len = rg->rg_end - rg->rg_start;
    if (offset > len || size > len - offset)
        return PT_STATUS_DENIED;                   /* vượt region, không tràn số */
    *va = rg->rg_start + offset;
    return PT_STATUS_OK;
}

/* phys của 1 byte User; SWAPPED -> nhờ P3 fault-in rồi dịch lại */
static int pt_user_byte_phys(struct pcb_t* caller, addr_t va, int is_write, addr_t* phys)
{
    struct pt_translation t;
    int rc = pt_translate_user_for_copy(caller, va, is_write, &t);

    if (rc == PT_STATUS_SWAPPED) {
        if (mm_handle_swapped_page(caller, (addr_t)MM64_VA_PAGE_BASE(va)) != 0)
            return PT_STATUS_ERROR;
        rc = pt_translate_user_for_copy(caller, va, is_write, &t);
        if (rc == PT_STATUS_SWAPPED)
            return PT_STATUS_ERROR;
    }
    if (rc != PT_STATUS_OK)
        return rc;
    *phys = pt_translation_phys(&t);
    return PT_STATUS_OK;
}


static int pt_kregion_span(struct pcb_t* caller, int krgid, addr_t offset,
    addr_t size, addr_t* va)
{
    struct kmem_region* rec;

    if (krgid < 0 || krgid >= PAGING_MAX_SYMTBL_SZ)
        return PT_STATUS_ERROR;
    rec = kregion_lookup(caller->krnl, caller->pid, krgid);
    if (rec == NULL)
        return PT_STATUS_DENIED;
    if (offset > rec->size || size > rec->size - offset)
        return PT_STATUS_DENIED;                   
    *va = rec->kva + offset;
    return PT_STATUS_OK;
}



static int pt_copy_user_kernel(struct pcb_t* caller, int urgid, int krgid,
    addr_t offset, addr_t size, int to_user)
{
    addr_t uva, kva, i;
    int rc;

    if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL ||
        caller->mm == NULL)
        return PT_STATUS_ERROR;

    pthread_mutex_lock(&mmvm_lock);

    /* 1 */
    rc = pt_region_span(caller->mm, urgid, offset, size, &uva);
    if (rc == PT_STATUS_OK)
        rc = pt_kregion_span(caller, krgid, offset, size, &kva);
    if (rc != PT_STATUS_OK || size == 0)
        goto out;

    /* 2: kiểm trang đầu + mỗi biên trang */
    for (i = 0; i < size; ) {
        struct pt_translation t;
        addr_t step_u = MM64_PAGE_SIZE - MM64_VA_OFFSET(uva + i);
        addr_t step_k = MM64_PAGE_SIZE - MM64_VA_OFFSET(kva + i);

        rc = pt_translate_va(caller, kva + i, to_user ? PT_KERNEL_READ : PT_KERNEL_WRITE, &t);
        if (rc == PT_STATUS_SWAPPED)
            rc = PT_STATUS_ERROR;                   
        if (rc != PT_STATUS_OK)
            goto out;
        rc = pt_translate_user_for_copy(caller, uva + i, to_user, &t);
        if (rc != PT_STATUS_OK && rc != PT_STATUS_SWAPPED)
            goto out;
        i += (step_u < step_k) ? step_u : step_k;  /* tới biên trang gần nhất */
    }

    /* 3 + 4 */
    for (i = 0; i < size; i++) {
        struct pt_translation kt;
        addr_t uphys;
        BYTE b;

        rc = pt_user_byte_phys(caller, uva + i, to_user, &uphys);
        if (rc != PT_STATUS_OK)
            goto out;
        rc = pt_translate_va(caller, kva + i, to_user ? PT_KERNEL_READ : PT_KERNEL_WRITE, &kt);
        if (rc != PT_STATUS_OK) {
            rc = PT_STATUS_ERROR;
            goto out;
        }

        if (to_user) {
            if (MEMPHY_read(caller->krnl->mram, pt_translation_phys(&kt), &b) != 0 ||
                MEMPHY_write(caller->krnl->mram, uphys, b) != 0) {
                rc = PT_STATUS_ERROR;
                goto out;
            }
        }
        else {
            if (MEMPHY_read(caller->krnl->mram, uphys, &b) != 0 ||
                MEMPHY_write(caller->krnl->mram, pt_translation_phys(&kt), b) != 0) {
                rc = PT_STATUS_ERROR;
                goto out;
            }
        }
        pt_update_access_flags(caller, uva + i, to_user);
    }
    rc = PT_STATUS_OK;

out:
    pthread_mutex_unlock(&mmvm_lock);
    return rc;
}

int libkmem_copy_from_user(struct pcb_t* caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
    return pt_copy_user_kernel(caller, (int)source, (int)destination,
        (addr_t)offset, (addr_t)size, 0);
}

int libkmem_copy_to_user(struct pcb_t* caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
    return pt_copy_user_kernel(caller, (int)destination, (int)source,
        (addr_t)offset, (addr_t)size, 1);
}

static int pt_zero_byte_phys(struct pcb_t* caller, int is_kernel, addr_t va, addr_t* phys)
{
    struct pt_translation t;
    int rc;

    if (!is_kernel)
        return pt_user_byte_phys(caller, va, 1, phys);

    rc = pt_translate_va(caller, va, PT_KERNEL_WRITE, &t);
    if (rc == PT_STATUS_SWAPPED)
        return PT_STATUS_ERROR;                    
    if (rc != PT_STATUS_OK)
        return rc;
    *phys = pt_translation_phys(&t);
    return PT_STATUS_OK;
}

int vm_zero_allocated_range(struct pcb_t* caller, int is_kernel, addr_t start, addr_t end)
{
    addr_t va;

    if (caller == NULL || caller->krnl == NULL || caller->krnl->mram == NULL)
        return PT_STATUS_ERROR;
    if (is_kernel ? (caller->krnl->mm == NULL) : (caller->mm == NULL))
        return PT_STATUS_ERROR;
    if (start > end)
        return PT_STATUS_ERROR;
    if (start == end)
        return PT_STATUS_OK;
    if (!mm_is_canonical_va(start) || !mm_is_canonical_va(end - 1) ||
        (((uint64_t)start ^ (uint64_t)(end - 1)) & MM64_VA_SIGN_MASK))
        return PT_STATUS_INVALID_VA;

    /* 1. kiểm trước, mỗi trang một lần */
    for (va = start; va < end; ) {
        struct pt_translation t;
        int rc;
        addr_t next = (addr_t)MM64_VA_PAGE_BASE(va) + MM64_PAGE_SIZE;

        if (is_kernel) {
            rc = pt_translate_va(caller, va, PT_KERNEL_WRITE, &t);
            if (rc == PT_STATUS_SWAPPED)
                rc = PT_STATUS_ERROR;
        }
        else {
            rc = pt_translate_user_for_copy(caller, va, 1, &t);
            if (rc == PT_STATUS_SWAPPED)
                rc = PT_STATUS_OK;
        }
        if (rc != PT_STATUS_OK)
            return rc;
        if (next <= va)                           
            break;
        va = next;
    }

    /* 2. ghi 0 */
    for (va = start; va < end; va++) {
        addr_t phys;
        int rc = pt_zero_byte_phys(caller, is_kernel, va, &phys);

        if (rc != PT_STATUS_OK)
            return rc;                               
        if (MEMPHY_write(caller->krnl->mram, phys, 0) != 0)
            return PT_STATUS_ERROR;
        if (!is_kernel)
            pt_update_access_flags(caller, va, 1);
    }
    return PT_STATUS_OK;
}

/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 * OS truy cập region User của caller: cùng đường với __read
 * (kiểm region, nạp trang SWAPPED, kiểm quyền, cập nhật A/D).
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy

    return __read(caller, vmaid, rgid, offset, data);
}


/*__write_user_mem - write a user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 * OS truy cập region User của caller: cùng đường với __read
 * (kiểm region, nạp trang SWAPPED, kiểm quyền, cập nhật A/D).
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy
    return __write(caller, vmaid, rgid, offset, value);
}

struct pt_cleanup_ctx {
    addr_t* va;
    size_t  n, cap;
    int     oom;
};
static enum pt_iter_action pt_cleanup_collect(addr_t va,
                                              const struct pt_mapping_info *mi,
                                              void *arg)
{
  struct pt_cleanup_ctx *c = arg;
  (void)mi;

  if (c->n == c->cap) {
    size_t ncap = c->cap ? c->cap * 2 : 64;
    addr_t *nv = realloc(c->va, ncap * sizeof(*nv));
    if (nv == NULL) {
      c->oom = 1;
      return PT_ITER_STOP;
    }
    c->va = nv;
    c->cap = ncap;
  }
  c->va[c->n++] = va;
  return PT_ITER_CONTINUE;
}
static struct memphy_struct* pt_swap_dev(struct krnl_t* krnl, int swptyp)
{
    if (krnl == NULL || krnl->mswp == NULL
        || swptyp < 0 || swptyp >= PAGING_MAX_MMSWP)
        return NULL;
    return krnl->mswp[swptyp];
}
/*
 * free_pcb_memph - thu hồi TOÀN BỘ bộ nhớ User của một process
 *@caller: process vừa kết thúc
 *
 *   1. pt_iterate_mappings gom VA của mọi mapping (callback chỉ đọc)
 *   2. với từng VA: pt_unmap_one (xóa PTE + prune TRƯỚC) rồi mới trả
 *        PRESENT -> frame về krnl->mram
 *        SWAPPED -> slot về krnl->mswp[swptyp]
 *        INVALID -> bỏ qua (không có gì hợp lệ để trả)
 *      Mỗi mapping chỉ được unmap thành công một lần -> không Double Free.
 *   3. dọn fifo_pgn còn sót, pt_destroy_tree (giải phóng cây kể cả root).
 *   Kernel PGD dùng chung KHÔNG bị đụng.
 *   Gọi lại lần 2 (mm->pgd đã NULL) -> trả 0, không làm gì.
 * Trả 0 nếu thu hồi hết; -1 nếu cây hỏng / hết Host memory (phần đã gom
 * được vẫn được thu hồi, cây vẫn bị hủy).
 */
int free_pcb_memph(struct pcb_t *caller)
{
    struct pt_cleanup_ctx c = { NULL, 0, 0, 0 };
    int it, ret = 0;
    size_t i;

    if (caller == NULL || caller->krnl == NULL || caller->mm == NULL)
        return -1;

    pthread_mutex_lock(&mmvm_lock);

    if (caller->mm->pgd == NULL) {               
        pthread_mutex_unlock(&mmvm_lock);
        return 0;
    }

    /* 1 */
    it = pt_iterate_mappings(caller, pt_cleanup_collect, &c);
    if (it == PT_ITER_ERROR || c.oom)
        ret = -1;

    /* 2 */
    for (i = 0; i < c.n; i++) {
        struct pt_mapping_info old;

        if (pt_unmap_one(caller, c.va[i], &old) != PT_STATUS_OK)
            continue;                                /* INVALID / hỏng: không trả gì */
        if (old.state == PT_MAP_PRESENT)
            MEMPHY_put_freefp(caller->krnl->mram, old.location.fpn);
        else if (old.state == PT_MAP_SWAPPED)
            MEMPHY_put_freefp(pt_swap_dev(caller->krnl, old.location.swap.swptyp),
                old.location.swap.swpoff);
    }
    free(c.va);

    /* 3 */
    while (caller->mm->fifo_pgn != NULL) {
        struct pgn_t* nx = caller->mm->fifo_pgn->pg_next;
        free(caller->mm->fifo_pgn);
        caller->mm->fifo_pgn = nx;
    }
    pt_destroy_tree(caller->mm);

    pthread_mutex_unlock(&mmvm_lock);
    return ret;
}


/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
    struct pgn_t** pp;
    struct pgn_t* pg;

    /* FIFO: enlist_pgn_node chèn ở ĐẦU -> trang cũ nhất là nút CUỐI */
    if (mm == NULL || retpgn == NULL || mm->fifo_pgn == NULL)
        return -1;

    pp = &mm->fifo_pgn;
    while ((*pp)->pg_next != NULL)
        pp = &(*pp)->pg_next;

    pg = *pp;
    *retpgn = pg->pgn;
    *pp = NULL;                        
    free(pg);
    return 0;
}

/*get_free_vmrg_area - get a free vm region
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@size: allocated size
 *
 */
int get_free_vmrg_area(struct pcb_t *caller, int vmaid, int size, struct vm_rg_struct *newrg)
{
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);

  /* NULL-safe: chua co VMA thi khong co vung free */
  if (cur_vma == NULL)
    return -1;

  struct vm_rg_struct *rgit = cur_vma->vm_freerg_list;

  if (rgit == NULL)
    return -1;

  /* Probe unintialized newrg */
  newrg->rg_start = newrg->rg_end = -1;
  newrg->vmaid = vmaid;
  newrg->active = 1;

  /* Traverse on list of free vm region to find a fit space */
  while (rgit != NULL)
  {
    if (rgit->rg_start + size <= rgit->rg_end)
    { /* Current region has enough space */
      newrg->rg_start = rgit->rg_start;
      newrg->rg_end = rgit->rg_start + size;

      /* Update left space in chosen region */
      if (rgit->rg_start + size < rgit->rg_end)
      {
        rgit->rg_start = rgit->rg_start + size;
      }
      else
      { /*Use up all space, remove current node */
        /*Clone next rg node */
        struct vm_rg_struct *nextrg = rgit->rg_next;

        /*Cloning */
        if (nextrg != NULL)
        {
          rgit->rg_start = nextrg->rg_start;
          rgit->rg_end = nextrg->rg_end;

          rgit->rg_next = nextrg->rg_next;

          free(nextrg);
        }
        else
        {                                /*End of free list */
          rgit->rg_start = rgit->rg_end; // dummy, size 0 region
          rgit->rg_next = NULL;
        }
      }
      break;
    }
    else
    {
      rgit = rgit->rg_next; // Traverse next rg
    }
  }

  if (newrg->rg_start == -1) // new region not found
    return -1;

  return 0;
}

// #endif
