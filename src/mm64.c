/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

/*
 * PAGING based Memory Management
 * Memory management unit mm/mm.c
 */

#include "mm64.h"
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

#if defined(MM64)

/*
 * init_pte - Initialize PTE entry
 */
int init_pte(addr_t *pte,
             int pre,    // present
             addr_t fpn,    // FPN
             int drt,    // dirty
             int swp,    // swap
             int swptyp, // swap type
             addr_t swpoff) // swap offset
{
    mm64_pte_t val;
    const uint64_t perm = MM64_PTE_DEFAULT_USER & MM64_PTE_PERM_MASK;

    if (pte == NULL)
        return -1;

    if (pre != 0 && swp != 0)
        return -1;


    if (swp != 0) {
        /* SWAPPED không lưu ACCESSED/DIRTY (đã chốt ở việc 1.2) */
        if (drt != 0)
            return -1;
        if (pte_encode_swapped(swptyp, swpoff, perm, &val) != 0)
            return -1;
    }
    else if (pre != 0) {
        uint64_t flags = perm | (drt ? MM64_PTE_DIRTY : 0);
        if (pte_encode_present(fpn, flags, &val) != 0)
            return -1;
    }
    else {
        val = 0;  /* EMPTY */
    }

    *pte = (addr_t)val;
    return 0;
}

/*
 * mm_is_canonical_va - Check if a virtual address is canonical
 * @va: virtual address
 * Return: 1 if canonical, 0 otherwise
 */

int mm_is_canonical_va(addr_t va)
{
    uint64_t top = ((uint64_t)va) >> MM64_VA_SIGN_BIT;   /* bits 63..56 */
    return (top == 0) || (top == 0xFFULL);
}


/*
 * get_pd_from_pagenum - Parse address to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table 
 */
int get_pd_from_address(addr_t addr, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
    if (!pgd || !p4d || !pud || !pmd || !pt)
        return -1;
    if (!mm_is_canonical_va(addr))
        return -1;

    *pgd = (addr_t)MM64_VA_INDEX(addr, MM64_LVL_PGD);
    *p4d = (addr_t)MM64_VA_INDEX(addr, MM64_LVL_P4D);
    *pud = (addr_t)MM64_VA_INDEX(addr, MM64_LVL_PUD);
    *pmd = (addr_t)MM64_VA_INDEX(addr, MM64_LVL_PMD);
    *pt = (addr_t)MM64_VA_INDEX(addr, MM64_LVL_PT);

    return 0;
}

/*
 * get_pd_from_pagenum - Parse page number to 5 page directory level
 * @pgn   : pagenumer
 * @pgd   : page global directory
 * @p4d   : page level directory
 * @pud   : page upper directory
 * @pmd   : page middle directory
 * @pt    : page table 
 */
int get_pd_from_pagenum(addr_t pgn, addr_t* pgd, addr_t* p4d, addr_t* pud, addr_t* pmd, addr_t* pt)
{
    if ((uint64_t)pgn > MM64_PGN_MAX)
        return -1;

    return get_pd_from_address(mm64_pgn_to_va(pgn), pgd, p4d, pud, pmd, pt);
}

static struct {
    unsigned long live[MM64_PT_LEVELS];  /* số bảng đang sống theo cấp */
    unsigned long allocs;
    unsigned long frees;
    unsigned long walks;
} g_pt_stats;

unsigned long pt_host_tables_live(void)
{
    unsigned long n = 0;
    unsigned l;
    for (l = 0; l < MM64_PT_LEVELS; l++)
        n += g_pt_stats.live[l];
    return n;
}

#ifdef MM64_TESTING
/* Fault injection cho test rollback: lần cấp phát thứ N (đếm từ 1) thất bại */
static long g_pt_fail_after = -1;
void pt_test_fail_alloc_after(long n) { g_pt_fail_after = n; }
#endif

/*  * pt_table_alloc - Allocate a new page table for a given level
 * @level: the level of the page table (0 for PGD, 1 for P4D, etc.)
 * Return: pointer to the allocated page table, or NULL on failure
 */
static mm64_table_t* pt_table_alloc(unsigned level)
{
    mm64_table_t* t;

#ifdef MM64_TESTING
    if (g_pt_fail_after >= 0 && g_pt_fail_after-- == 0)
        return NULL;
#endif

    if (posix_memalign((void**)&t, MM64_PAGE_SIZE, sizeof(mm64_table_t)) != 0)
        return NULL;

    memset(t, 0, sizeof(*t));
    g_pt_stats.live[level]++;
    g_pt_stats.allocs++;
    return t;
}

static void pt_table_free(mm64_table_t* t, unsigned level)
{
    if (t == NULL)
        return;
    free(t);
    g_pt_stats.live[level]--;
    g_pt_stats.frees++;
}

struct pt_walk {
    mm64_table_t* tbl[MM64_PT_LEVELS]; /* tbl[0]=PGD ... tbl[4]=PT, NULL nếu chưa có */
    unsigned      idx[MM64_PT_LEVELS]; /* chỉ số trong từng bảng                     */
    unsigned      created;             /* bit l = 1: bảng cấp l do walk này tạo       */
    mm64_pte_t* pte;                 /* &PT[idx[4]], NULL nếu chưa tới lá          */
};

/* pde_count - get count of a PDE entry
 * @pde: the PDE entry
 * Return: the count value (0..512)
 */
static int pt_count_inc(struct pt_walk* w, unsigned l)
{
    mm64_pde_t* pe;
    unsigned c;

    if (l == 0)
        return 0;
    pe = &w->tbl[l - 1]->entries[w->idx[l - 1]];
    c = pde_count(*pe);
    if (c >= MM64_PDE_COUNT_MAX)
        return -1;                      
    *pe = (*pe & ~MM64_PDE_COUNT_MASK) | (mm64_pde_t)(c + 1);
    return 0;
}

static int pt_count_dec(struct pt_walk* w, unsigned l)
{
    mm64_pde_t* pe;
    unsigned c;

    if (l == 0)
        return 0;
    pe = &w->tbl[l - 1]->entries[w->idx[l - 1]];
    c = pde_count(*pe);
    if (c == 0)
        return -1;                    
    *pe = (*pe & ~MM64_PDE_COUNT_MASK) | (mm64_pde_t)(c - 1);
    return 0;
}

/* bảng có thật sự toàn 0 không (không tin used_count) */
static int pt_table_is_empty(const mm64_table_t* t)
{
    unsigned i;
    for (i = 0; i < MM64_PT_ENTRIES; i++)
        if (t->entries[i] != 0)
            return 0;
    return 1;
}

/*
 * pt_unlink_empty_table - gỡ bảng cấp l (1..4) trên nhánh của walk
 *   Điều kiện (kiểm hết TRƯỚC khi sửa gì):
 *     - used_count của bảng = 0  VÀ  bảng thật sự toàn 0
 *     - nếu cha không phải root: used_count của cha > 0
 *   Đạt -> free bảng, xóa entry ở cha, giảm count của cha, trả 0.
 *   Không đạt -> không đổi gì, trả -1 (cây hỏng hoặc bảng còn dùng).
 */
static int pt_unlink_empty_table(struct pt_walk* w, unsigned l)
{
    mm64_pde_t* pe = &w->tbl[l - 1]->entries[w->idx[l - 1]];

    if (pde_count(*pe) != 0 || !pt_table_is_empty(w->tbl[l]))
        return -1;
    if (l >= 2 && pde_count(w->tbl[l - 2]->entries[w->idx[l - 2]]) == 0)
        return -1;

    pt_table_free(w->tbl[l], l);
    w->tbl[l] = NULL;
    w->created &= ~(1u << l);
    if (l == MM64_LVL_PT)
        w->pte = NULL;

    *pe = 0;
    pt_count_dec(w, l - 1);          
    return 0;
}

/*
 * pt_prune_empty - Immediate Pruning
 *   Đi từ cấp thấp nhất đang có lên trên, gỡ bảng rỗng; dừng ở bảng đầu
 *   tiên còn entry. Không bao giờ gỡ root. Gặp count lệch với nội dung
 *   thật thì dừng (giữ bảng) thay vì free một bảng còn mapping.
 */
static void pt_prune_empty(struct pt_walk* w)
{
    int l;

    for (l = MM64_PT_LEVELS - 1; l >= 1; l--) {
        if (w->tbl[l] == NULL)
            continue;                     
        if (pt_unlink_empty_table(w, (unsigned)l) != 0)
            break;
    }
}

/*
 * pt_rollback_created_branches - hoàn tác nhánh mới tạo
 *   Chỉ gỡ bảng có bit trong w->created và còn rỗng. Bảng đã có trước
 *   walk không bao giờ bị đụng tới.
 *   Trả 0 nếu hoàn tác hết; -1 nếu gặp bảng mới tạo nhưng đã có entry
 *   (đã commit -> phải unmap, không phải rollback).
 */
static int pt_rollback_created_branches(struct pt_walk* w)
{
    int l;

    for (l = MM64_PT_LEVELS - 1; l >= 1; l--) {
        if (!(w->created & (1u << l)) || w->tbl[l] == NULL)
            continue;
        if (pt_unlink_empty_table(w, (unsigned)l) != 0)
            return -1;
    }
    return 0;
}


/* pt_walk_internal - Walk the page table to find the PTE for a given virtual address
 * @root  : root page table (PGD)
 * @va    : virtual address
 * @create: if non-zero, create intermediate tables if they don't exist
 * @w     : output structure to hold walk information
 * Return: PT_STATUS_OK on success, or an error code
 */
static int pt_walk_internal(mm64_table_t* root, addr_t va, int create,
    struct pt_walk* w)
{
    unsigned l;

    memset(w, 0, sizeof(*w));
    if (root == NULL)
        return PT_STATUS_ERROR;
    if (!mm_is_canonical_va(va))
        return PT_STATUS_INVALID_VA;

    for (l = 0; l < MM64_PT_LEVELS; l++)
        w->idx[l] = (unsigned)MM64_VA_INDEX(va, l);
    w->tbl[0] = root;
    g_pt_stats.walks++;

    for (l = 0; l < MM64_PT_LEVELS - 1; l++) {
        mm64_pde_t* e = &w->tbl[l]->entries[w->idx[l]];

        if (*e == 0) {
            mm64_table_t* child;

            if (!create)
                return PT_STATUS_UNMAPPED;

            child = pt_table_alloc(l + 1);
            if (child == NULL) {
                pt_rollback_created_branches(w);
                return PT_STATUS_ERROR;
            }
            *e = pde_make(child, 0);
            w->created |= 1u << (l + 1);
            if (pt_count_inc(w, l) != 0) {
                pt_rollback_created_branches(w);
                return PT_STATUS_ERROR;
            }
        }
        else if (pde_validate(*e) != 0) {
            pt_rollback_created_branches(w);
            return PT_STATUS_ERROR;
        }

        w->tbl[l + 1] = pde_child(*e);
    }

    w->pte = &w->tbl[MM64_LVL_PT]->entries[w->idx[MM64_LVL_PT]];
    return PT_STATUS_OK;
}

static struct mm_struct* pt_resolve_mm(struct mm_struct* mm, struct pcb_t* caller)
{
    if (mm != NULL)
        return mm;
    return (caller != NULL) ? caller->mm : NULL;
}

static int pt_is_kernel_mm(struct mm_struct* mm, struct pcb_t* caller)
{
    return caller != NULL && caller->krnl != NULL && mm == caller->krnl->mm;
}

/* -------- root ------------------------------------------------------- */

/* pte_encode_swapped - tạo Leaf PTE SWAPPED
 * @swptyp : swap type
 * @swpoff : swap offset
 * @flags  : tập con của RW | USER | NX
 * @out    : nơi lưu kết quả
 */
int pt_init_root(struct mm_struct* mm)
{
    mm64_table_t* root;

    if (mm == NULL)
        return PT_STATUS_ERROR;

    root = pt_table_alloc(MM64_LVL_PGD);
    if (root == NULL)
        return PT_STATUS_ERROR;

    mm->pgd = (addr_t*)root;
    mm->fifo_pgn = NULL;
    mm->p4d = NULL;
    mm->pud = NULL;
    mm->pmd = NULL;
    mm->pt = NULL;
    return PT_STATUS_OK;
}

/* pt_destroy_level - recursively destroy a page table level
 * @t    : the page table to destroy
 * @level: the level of the page table (0 for PGD, 1 for P4D, etc.)
 */
static void pt_destroy_level(mm64_table_t* t, unsigned level)
{
    unsigned i;

    if (level < MM64_LVL_PT) {
        for (i = 0; i < MM64_PT_ENTRIES; i++) {
            mm64_pde_t e = t->entries[i];
            if (e == 0)
                continue;
            if (pde_validate(e) != 0)
				continue;                    // skip invalid entries
            pt_destroy_level(pde_child(e), level + 1);
        }
    }
    pt_table_free(t, level);
}

/* pt_destroy_tree - destroy the entire page table tree for a given mm_struct
 * @mm: the mm_struct whose page table tree to destroy
 * Return: PT_STATUS_OK on success, PT_STATUS_ERROR on failure
 */
int pt_destroy_tree(struct mm_struct* mm)
{
    if (mm == NULL)
        return PT_STATUS_ERROR;
    if (mm->pgd != NULL)
        pt_destroy_level((mm64_table_t*)mm->pgd, MM64_LVL_PGD);
    mm->pgd = NULL;
    return PT_STATUS_OK;
}

/* pte_verify_level - recursively verify a page table level
 * @t    : the page table to verify
 * @level : the level of the page table(0 for PGD, 1 for P4D, etc.)
 * Return : number of invalid entries found
 */
static long pt_verify_level(const mm64_table_t* t, unsigned level)
{
    long bad = 0;
    unsigned i;

    if (((uintptr_t)t & MM64_PAGE_MASK) != 0)
        bad++;                                         /* không căn 4096 */

    for (i = 0; i < MM64_PT_ENTRIES; i++) {
        uint64_t e = t->entries[i];
        const mm64_table_t* c;
        unsigned j, nz = 0;

        if (e == 0)
            continue;
        if (level == MM64_LVL_PT) {
            if (pte_decode_state(e) == PT_MAP_INVALID)
                bad++;                                 /* leaf INVALID */
            continue;
        }
        if (pde_validate(e) != 0) {
            bad++;                                     /* entry trung gian hỏng */
            continue;
        }
        c = pde_child(e);
        for (j = 0; j < MM64_PT_ENTRIES; j++)
            nz += (c->entries[j] != 0);
        if (nz != pde_count(e))
            bad++;                                     /* used_count lệch */
        if (nz == 0)
            bad++;                                     /* bảng rỗng chưa prune */
        bad += pt_verify_level(c, level + 1);
    }
    return bad;
}

/*
 * pt_verify_tree - đếm số vi phạm bất biến của cây (0 = sạch)
 *   Kiểm: căn 4096, entry trung gian hợp lệ, used_count = số entry khác 0
 *   thật, không còn bảng rỗng ngoài root, không có leaf INVALID.
 *   Trả -1 nếu mm/root NULL. Chỉ đọc; bên gọi giữ Global MM Lock.
 */
long pt_verify_tree(struct mm_struct* mm)
{
    if (mm == NULL || mm->pgd == NULL)
        return -1;
    return pt_verify_level((const mm64_table_t*)mm->pgd, MM64_LVL_PGD);
}

static void pt_fifo_forget(struct mm_struct* mm, addr_t pgn);


/*
 * pte_set_swap - Set PTE entry for swapped page
 * @pte    : target page table entry (PTE)
 * @swptyp : swap type
 * @swpoff : swap offset
 *   P3 phải copy dữ liệu RAM -> SWAP TRƯỚC, và tự trả frame SAU khi hàm OK.
 *   Trang được gỡ khỏi fifo_pgn (nếu còn) — fifo chỉ chứa trang PRESENT.
 */
 
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{	
  struct mm_struct* mm;
  mm64_pte_t old, val;

  if (caller == NULL || caller->mm == NULL)
      return PT_STATUS_ERROR;
  mm = caller->mm;
  if (pt_is_kernel_mm(mm, caller))
      return PT_STATUS_DENIED;
  if ((uint64_t)pgn > MM64_PGN_MAX)
      return PT_STATUS_INVALID_VA;

  old = (mm64_pte_t)pte_get_entry(mm, pgn, caller);
  switch (pte_decode_state(old)) {
  case PT_MAP_PRESENT:
      break;
  case PT_MAP_EMPTY:
      return PT_STATUS_UNMAPPED;
  default:
      return PT_STATUS_ERROR;          /* SWAPPED sẵn hoặc INVALID */
  }
  if (!(old & MM64_PTE_USER))
      return PT_STATUS_DENIED;

  if (pte_encode_swapped(swptyp, swpoff, pte_decode_perm(old), &val) != 0)
      return PT_STATUS_ERROR;
  if (pte_set_entry(mm, pgn, val, caller) != PT_STATUS_OK)
      return PT_STATUS_ERROR;

  /* bất biến: fifo_pgn chỉ chứa trang User đang PRESENT */
  pt_fifo_forget(mm, pgn);
  return PT_STATUS_OK;
}

/*
 * pte_set_fpn - Set PTE entry for on-line page
 * @pte   : target page table entry (PTE)
 * @fpn   : frame page number (FPN)
 * @caller: caller
 */
int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn, struct pcb_t *caller)
{
  mm64_pte_t old, val;
  uint64_t perm;

  if (caller == NULL)
      return PT_STATUS_ERROR;
  mm = pt_resolve_mm(mm, caller);
  if (mm == NULL)
      return PT_STATUS_ERROR;
  if ((uint64_t)pgn > MM64_PGN_MAX)
      return PT_STATUS_INVALID_VA;

  old = (mm64_pte_t)pte_get_entry(mm, pgn, caller);
  switch (pte_decode_state(old)) {
  case PT_MAP_EMPTY:
      perm = (pt_is_kernel_mm(mm, caller) ? MM64_PTE_DEFAULT_KERNEL
          : MM64_PTE_DEFAULT_USER) & MM64_PTE_PERM_MASK;
      break;
  case PT_MAP_PRESENT:
  case PT_MAP_SWAPPED:
      perm = pte_decode_perm(old);
      break;
  default:
      return PT_STATUS_ERROR;
  }

  if (pte_encode_present(fpn, perm, &val) != 0)
      return PT_STATUS_ERROR;
  return pte_set_entry(mm, pgn, val, caller);
}


/* Get PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @caller: caller
 * @ret    : page table entry
 **/
addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
  
  struct pt_walk w;

  mm = pt_resolve_mm(mm, caller);
  if (mm == NULL || (uint64_t)pgn > MM64_PGN_MAX)
      return 0;

  if (pt_walk_internal((mm64_table_t*)mm->pgd, mm64_pgn_to_va(pgn), 0, &w)
      != PT_STATUS_OK)
      return 0;

  return (addr_t)*w.pte;

}

/* Set PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @ret    : page table entry
 **/
int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller)
{
  
  struct pt_walk w;
  enum pt_mapping_state new_st, old_st;
  int rc;

  mm = pt_resolve_mm(mm, caller);
  if (mm == NULL || mm->pgd == NULL)
      return PT_STATUS_ERROR;
  if ((uint64_t)pgn > MM64_PGN_MAX)
      return PT_STATUS_INVALID_VA;

  new_st = pte_decode_state((mm64_pte_t)pte_val);
  if (new_st == PT_MAP_INVALID)
      return PT_STATUS_ERROR;

  /* ---- xóa mapping ---- */
  if (new_st == PT_MAP_EMPTY) {
      rc = pt_walk_internal((mm64_table_t*)mm->pgd, mm64_pgn_to_va(pgn), 0, &w);
      if (rc == PT_STATUS_UNMAPPED)
          return PT_STATUS_OK;
      if (rc != PT_STATUS_OK)
          return rc;
      if (*w.pte == 0)
          return PT_STATUS_OK;

      if (pde_count(w.tbl[MM64_LVL_PMD]->entries[w.idx[MM64_LVL_PMD]]) == 0)
          return PT_STATUS_ERROR;

      *w.pte = 0;
      if (pt_count_dec(&w, MM64_LVL_PT) != 0)
          return PT_STATUS_ERROR;
      pt_prune_empty(&w);
      return PT_STATUS_OK;
  }

  /* ---- tạo / cập nhật mapping ---- */
  rc = pt_walk_internal((mm64_table_t*)mm->pgd, mm64_pgn_to_va(pgn), 1, &w);
  if (rc != PT_STATUS_OK)
      return rc;

  old_st = pte_decode_state(*w.pte);
  if (old_st == PT_MAP_INVALID) {
      pt_rollback_created_branches(&w);
      return PT_STATUS_ERROR;
  }

  if (old_st == PT_MAP_EMPTY) {
      if (pt_count_inc(&w, MM64_LVL_PT) != 0) {
          pt_rollback_created_branches(&w);
          return PT_STATUS_ERROR;
      }
  }
  *w.pte = (mm64_pte_t)pte_val;
  return PT_STATUS_OK;
}

/* pt_range_validate - check validate [va, va + pgnum * 4096) */
static int pt_range_validate(addr_t va, uint64_t pgnum)
{
    uint64_t last;

    if (!MM64_VA_PAGE_ALIGNED(va) || !mm_is_canonical_va(va))
        return PT_STATUS_INVALID_VA;
    if (pgnum == 0)
        return PT_STATUS_OK;
    if (pgnum - 1 > (UINT64_MAX - (uint64_t)va) / MM64_PAGE_SIZE)
        return PT_STATUS_INVALID_VA;              

    last = (uint64_t)va + (pgnum - 1) * MM64_PAGE_SIZE;
    if (!mm_is_canonical_va(last))
        return PT_STATUS_INVALID_VA;
    if ((last ^ (uint64_t)va) & MM64_VA_SIGN_MASK)
        return PT_STATUS_INVALID_VA;               
    return PT_STATUS_OK;
}

/*
 * pt_range_check_empty - mọi Leaf PTE trong vùng đều = 0 ? (chỉ đọc)
 *   Khi gặp bảng trung gian chưa có, nhảy qua cả khoảng nó phủ
 *   (PT thiếu = 2 MiB, PMD-table thiếu = 1 GiB, ...) thay vì walk từng trang.
 * @busy : nếu khác NULL, nhận VA của trang đầu tiên đã có mapping
 * Trả OK | EXISTS | INVALID_VA | ERROR (root NULL hoặc entry trung gian hỏng).
 */
static int pt_range_check_empty(mm64_table_t* root, addr_t va, uint64_t pgnum,
    addr_t* busy)
{
    uint64_t cur, remaining;
    int rc;

    rc = pt_range_validate(va, pgnum);
    if (rc != PT_STATUS_OK)
        return rc;
    if (root == NULL)
        return PT_STATUS_ERROR;
    if (pgnum == 0)
        return PT_STATUS_OK;

    cur = (uint64_t)va;
    remaining = pgnum;     

    while (remaining > 0) {
        struct pt_walk w;
        uint64_t step;                            

        rc = pt_walk_internal(root, (addr_t)cur, 0, &w);
        if (rc == PT_STATUS_OK) {
            if (*w.pte != 0) {
                if (busy)
                    *busy = (addr_t)cur;
                return PT_STATUS_EXISTS;
            }
            step = 1;
        }
        else if (rc == PT_STATUS_UNMAPPED) {
            
            int d = MM64_PT_LEVELS - 1;
            uint64_t span, off;

            while (d > 0 && w.tbl[d] == NULL)
                d--;
            span = UINT64_C(1) << MM64_LEVEL_SHIFT(d);
            off = cur & (span - 1);
            step = (span - off) >> MM64_PAGE_SHIFT;
        }
        else {
            return rc;                               /* ERROR */
        }

        if (step >= remaining)
            break;
        remaining -= step;
        cur += step * MM64_PAGE_SIZE;
    }
    return PT_STATUS_OK;
}

/*
 * vmap_pgd_memset - map a range of page at aligned address
 */
int vmap_pgd_memset(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz
                    int pgnum)                      // num of mapping page
{
    if (caller == NULL || caller->mm == NULL)
        return PT_STATUS_ERROR;
    if (pgnum < 0)
        return PT_STATUS_ERROR;

    return pt_range_check_empty((mm64_table_t*)caller->mm->pgd, addr,
        (uint64_t)pgnum, NULL);
}

/*pt_map_range - map a range of page at aligned address*/
static int pt_map_range(struct mm_struct* mm, addr_t va, uint64_t pgnum,
    struct framephy_struct* frames, uint64_t perm,
    int track_fifo)
{
    struct framephy_struct* fp;
    struct pgn_t* nodes = NULL, * nd;
    uint64_t i, n;
    int rc;

    if (mm == NULL || mm->pgd == NULL)
        return PT_STATUS_ERROR;
    if (perm & ~MM64_PTE_PERM_MASK)
        return PT_STATUS_ERROR;

    /* 1a. vùng hợp lệ và trống hoàn toàn */
    rc = pt_range_check_empty((mm64_table_t*)mm->pgd, va, pgnum, NULL);
    if (rc != PT_STATUS_OK)
        return rc;
    if (pgnum == 0)
        return PT_STATUS_OK;

    /* 1b. đúng pgnum frame, FPN hợp lệ */
    for (n = 0, fp = frames; fp != NULL; fp = fp->fp_next, n++)
        if ((uint64_t)fp->fpn > MM64_PTE_FPN_MAX)
            return PT_STATUS_ERROR;
    if (n != pgnum)
        return PT_STATUS_ERROR;

    /* 1c. cấp sẵn node FIFO để bước 3 không thể thất bại */
    if (track_fifo) {
        for (i = 0; i < pgnum; i++) {
            nd = malloc(sizeof(*nd));
            if (nd == NULL) {
                while (nodes) { nd = nodes->pg_next; free(nodes); nodes = nd; }
                return PT_STATUS_ERROR;
            }
            nd->pg_next = nodes;
            nodes = nd;
        }
    }

    /* 2. map từng trang */
    for (i = 0, fp = frames; i < pgnum; i++, fp = fp->fp_next) {
        addr_t pgn = mm64_va_to_pgn((addr_t)((uint64_t)va + i * MM64_PAGE_SIZE));
        mm64_pte_t val;

        pte_encode_present(fp->fpn, perm, &val);   /* đã kiểm ở 1b, không lỗi */
        rc = pte_set_entry(mm, pgn, val, NULL);
        if (rc != PT_STATUS_OK) {
            uint64_t j;
            for (j = 0; j < i; j++)
                pte_set_entry(mm, mm64_va_to_pgn((addr_t)((uint64_t)va + j * MM64_PAGE_SIZE)),
                    0, NULL);
            while (nodes) { nd = nodes->pg_next; free(nodes); nodes = nd; }
            return rc;
        }
    }

    /* 3. ghi nhận cho page replacement: đẩy lần lượt vào đầu list */
    for (i = 0; i < pgnum && nodes != NULL; i++) {
        nd = nodes;
        nodes = nodes->pg_next;
        nd->pgn = mm64_va_to_pgn((addr_t)((uint64_t)va + i * MM64_PAGE_SIZE));
        nd->pg_next = mm->fifo_pgn;
        mm->fifo_pgn = nd;
    }
    return PT_STATUS_OK;
}



/*
 * vmap_page_range - map a range of page at aligned address
 */
addr_t vmap_page_range(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz // start
                    int pgnum,                      // num of mapping page
                    struct framephy_struct *frames, // list of the mapped frames
                    struct vm_rg_struct *ret_rg)    // return mapped region, the real mapped fp
{                                                   // no guarantee all given pages are mapped
    int rc;

    if (caller == NULL || caller->mm == NULL || pgnum < 0)
        return (addr_t)(int)PT_STATUS_ERROR;

    rc = pt_map_range(caller->mm, addr, (uint64_t)pgnum, frames,
        MM64_PTE_DEFAULT_USER & MM64_PTE_PERM_MASK, 1);
    if (rc != PT_STATUS_OK)
        return (addr_t)(int)rc;

    if (ret_rg != NULL) {
        ret_rg->rg_start = addr;
        ret_rg->rg_end = (addr_t)((uint64_t)addr + (uint64_t)pgnum * MM64_PAGE_SIZE);
    }
    return 0;

  return 0;
}

static void pt_fifo_forget(struct mm_struct* mm, addr_t pgn)
{
    struct pgn_t** pp = &mm->fifo_pgn;

    while (*pp != NULL) {
        if ((*pp)->pgn == pgn) {
            struct pgn_t* dead = *pp;
            *pp = dead->pg_next;
            free(dead);
            return;
        }
        pp = &(*pp)->pg_next;
    }
}

static int pt_unmap_one_mm(struct mm_struct* mm, addr_t va,
    struct pt_mapping_info* old)
{
    struct pt_walk w;
    struct pt_mapping_info info;
    mm64_pte_t pte;
    int rc;

    if (mm == NULL || mm->pgd == NULL)
        return PT_STATUS_ERROR;
    if (!MM64_VA_PAGE_ALIGNED(va) || !mm_is_canonical_va(va))
        return PT_STATUS_INVALID_VA;

    rc = pt_walk_internal((mm64_table_t*)mm->pgd, va, 0, &w);
    if (rc != PT_STATUS_OK)
        return rc;                                

    pte = *w.pte;
    pte_decode_info(pte, &info);
    if (info.state == PT_MAP_EMPTY)
        return PT_STATUS_UNMAPPED;                  /* Strict Unmap */
    if (info.state == PT_MAP_INVALID)
        return PT_STATUS_ERROR;                     
    if (pde_count(w.tbl[MM64_LVL_PMD]->entries[w.idx[MM64_LVL_PMD]]) == 0)
        return PT_STATUS_ERROR;                   

    /* commit: xóa PTE, giảm count, prune ngay, quên trong FIFO */
    *w.pte = 0;
    pt_count_dec(&w, MM64_LVL_PT);
    pt_prune_empty(&w);
    pt_fifo_forget(mm, mm64_va_to_pgn(va));

    if (old != NULL)
        *old = info;
    return PT_STATUS_OK;
}

/*
 * pt_unmap_one - unmap a page at aligned address
 * @caller : caller
 * @va     : virtual address to unmap
 * @old    : returned mapping info of the unmapped page
 */

int pt_unmap_one(struct pcb_t* caller, addr_t va, struct pt_mapping_info* old)
{
    if (caller == NULL)
        return PT_STATUS_ERROR;
    return pt_unmap_one_mm(caller->mm, va, old);
}

enum pt_op { PT_OP_READ, PT_OP_WRITE, PT_OP_EXEC };

static int pt_check_perm(mm64_pte_t pte, int kernel, enum pt_op op)
{
    if (!kernel && !(pte & MM64_PTE_USER))
        return PT_STATUS_DENIED;
    if (op == PT_OP_WRITE && !(pte & MM64_PTE_RW))
        return PT_STATUS_DENIED;
    if (op == PT_OP_EXEC) {
        if (pte & MM64_PTE_NX)
            return PT_STATUS_DENIED;
        if (kernel && (pte & MM64_PTE_USER))
            return PT_STATUS_DENIED;
    }
    return PT_STATUS_OK;
}

static int pt_translate_mm(struct mm_struct* mm, addr_t va, int kernel,
    enum pt_op op, struct pt_translation* out)
{
    struct pt_walk w;
    mm64_pte_t pte;
    int rc;

    if (mm == NULL || mm->pgd == NULL)
        return PT_STATUS_ERROR;
    if (!mm_is_canonical_va(va))
        return PT_STATUS_INVALID_VA;

    rc = pt_walk_internal((mm64_table_t*)mm->pgd, va, 0, &w);
    if (rc != PT_STATUS_OK)
        return rc;                                   /* UNMAPPED / ERROR */

    pte = *w.pte;
    switch (pte_decode_state(pte)) {
    case PT_MAP_EMPTY:
        return PT_STATUS_UNMAPPED;
    case PT_MAP_INVALID:
        return PT_STATUS_ERROR;
    default:
        break;
    }

    rc = pt_check_perm(pte, kernel, op);
    if (rc != PT_STATUS_OK)
        return rc;

    if (out != NULL) {
        out->offset = (uint16_t)MM64_VA_OFFSET(va);
        if (pte & MM64_PTE_PRESENT) {
            out->fpn = pte_decode_fpn(pte);
            out->swptyp = 0;
            out->swpoff = 0;
        }
        else {
            out->fpn = 0;
            out->swptyp = pte_decode_swptyp(pte);
            out->swpoff = pte_decode_swpoff(pte);
        }
    }
    return (pte & MM64_PTE_PRESENT) ? PT_STATUS_OK : PT_STATUS_SWAPPED;
}

int pt_translate_va(struct pcb_t* caller, addr_t va, enum pt_access_type access,
    struct pt_translation* out)
{
    struct mm_struct* mm;
    int kernel;
    enum pt_op op;

    if (caller == NULL)
        return PT_STATUS_ERROR;

    switch (access) {
    case PT_USER_READ:    kernel = 0; op = PT_OP_READ;  break;
    case PT_USER_WRITE:   kernel = 0; op = PT_OP_WRITE; break;
    case PT_USER_EXEC:    kernel = 0; op = PT_OP_EXEC;  break;
    case PT_KERNEL_READ:  kernel = 1; op = PT_OP_READ;  break;
    case PT_KERNEL_WRITE: kernel = 1; op = PT_OP_WRITE; break;
    case PT_KERNEL_EXEC:  kernel = 1; op = PT_OP_EXEC;  break;
    default:
        return PT_STATUS_INVALID_VA;
    }

    if (kernel)
        mm = (caller->krnl != NULL) ? caller->krnl->mm : NULL;
    else
        mm = caller->mm;

    return pt_translate_mm(mm, va, kernel, op, out);
}

int pt_translate_user_for_copy(struct pcb_t* caller, addr_t va, int is_write,
    struct pt_translation* out)
{
    if (caller == NULL)
        return PT_STATUS_ERROR;
    return pt_translate_mm(caller->mm, va, 1,
        is_write ? PT_OP_WRITE : PT_OP_READ, out);
}

int pt_update_access_flags(struct pcb_t* caller, addr_t va, int is_write)
{
    struct pt_walk w;
    mm64_pte_t pte;
    int rc;

    if (caller == NULL || caller->mm == NULL || caller->mm->pgd == NULL)
        return PT_STATUS_ERROR;
    if (!mm_is_canonical_va(va))
        return PT_STATUS_INVALID_VA;

    rc = pt_walk_internal((mm64_table_t*)caller->mm->pgd, va, 0, &w);
    if (rc != PT_STATUS_OK)
        return rc;

    pte = *w.pte;
    switch (pte_decode_state(pte)) {
    case PT_MAP_PRESENT:
        break;
    case PT_MAP_SWAPPED:
        return PT_STATUS_SWAPPED;
    case PT_MAP_EMPTY:
        return PT_STATUS_UNMAPPED;
    default:
        return PT_STATUS_ERROR;
    }

    if (is_write) {
        if (!(pte & MM64_PTE_RW))
            return PT_STATUS_DENIED;
        *w.pte = pte | MM64_PTE_ACCESSED | MM64_PTE_DIRTY;
    }
    else {
        *w.pte = pte | MM64_PTE_ACCESSED;
    }
    return PT_STATUS_OK;
}

static addr_t pt_va_from_index(const unsigned idx[MM64_PT_LEVELS])
{
    uint64_t va = 0;
    unsigned l;

    for (l = 0; l < MM64_PT_LEVELS; l++)
        va |= (uint64_t)idx[l] << MM64_LEVEL_SHIFT(l);
    if (va & MM64_VA_SIGN_MASK)
        va |= MM64_VA_CANON_MASK;
    return (addr_t)va;
}

/* duyệt đệ quy một bảng; trả PT_ITER_DONE / STOPPED / ERROR */
static int pt_iterate_level(const mm64_table_t* t, unsigned level,
    unsigned idx[MM64_PT_LEVELS],
    pt_mapping_callback cb, void* ctx)
{
    unsigned i;

    for (i = 0; i < MM64_PT_ENTRIES; i++) {
        uint64_t e = t->entries[i];

        if (e == 0)
            continue;                               
        idx[level] = i;

        if (level == MM64_LVL_PT) {
            struct pt_mapping_info mi;
            pte_decode_info((mm64_pte_t)e, &mi);
            if (cb(pt_va_from_index(idx), &mi, ctx) == PT_ITER_STOP)
                return PT_ITER_STOPPED;
        }
        else {
            int rc;
            if (pde_validate(e) != 0)
                return PT_ITER_ERROR;                 
            rc = pt_iterate_level(pde_child(e), level + 1, idx, cb, ctx);
            if (rc != PT_ITER_DONE)
                return rc;
        }
    }
    return PT_ITER_DONE;
}

int pt_iterate_mappings(struct pcb_t* caller, pt_mapping_callback callback, void* ctx)
{
    unsigned idx[MM64_PT_LEVELS] = { 0 };

    if (caller == NULL || caller->mm == NULL || caller->mm->pgd == NULL ||
        callback == NULL)
        return PT_ITER_ERROR;
    return pt_iterate_level((const mm64_table_t*)caller->mm->pgd,
        MM64_LVL_PGD, idx, callback, ctx);
}

/*
 * pt_swapin_commit - commit PTE sau khi P3 đã copy SWAP -> RAM (việc 32)
 * @caller  : process SỞ HỮU mapping; thao tác trên caller->mm
 * @va      : VA bất kỳ trong trang (offset bị bỏ qua)
 * @new_fpn : frame đã chứa dữ liệu; 0 .. MM64_PTE_FPN_MAX (FPN 0 hợp lệ)
 *
 *   Chỉ SWAPPED -> PRESENT. Giữ RW/USER/NX, ACCESSED = DIRTY = 0.
 *   used_count không đổi. Trang được đưa lại vào fifo_pgn (mới nhất), bỏ
 *   bản trùng nếu có -> P3 KHÔNG tự enlist_pgn_node nữa.
 *   KHÔNG giải phóng SWAP slot, KHÔNG đụng frame: P3 trả SWAP slot SAU khi
 *   hàm trả OK (dùng swptyp/swpoff đã đọc từ pt_translate_va trước đó).
 *
 * Trả OK | UNMAPPED (ô trống) | EXISTS (đã PRESENT) | DENIED (Kernel mm)
 *    | INVALID_VA | ERROR (tham số, FPN tràn, ô INVALID, hết Host memory).
 * Lỗi: không đổi gì. Bên gọi giữ Global MM Lock.
 */ 
 
int pt_swapin_commit(struct pcb_t* caller, addr_t va, addr_t new_fpn)
{
    struct mm_struct* mm;
    struct pt_walk w;
    struct pgn_t* node;
    mm64_pte_t old, val;
    addr_t pgn;
    int rc;

    if (caller == NULL || caller->mm == NULL || caller->mm->pgd == NULL)
        return PT_STATUS_ERROR;
    mm = caller->mm;
    if (pt_is_kernel_mm(mm, caller))
        return PT_STATUS_DENIED;                   /* Kernel không bao giờ bị swap */
    if (!mm_is_canonical_va(va))
        return PT_STATUS_INVALID_VA;

    rc = pt_walk_internal((mm64_table_t*)mm->pgd, va, 0, &w);
    if (rc != PT_STATUS_OK)
        return rc;

    old = *w.pte;
    switch (pte_decode_state(old)) {
    case PT_MAP_SWAPPED:
        break;
    case PT_MAP_EMPTY:
        return PT_STATUS_UNMAPPED;
    case PT_MAP_PRESENT:
        return PT_STATUS_EXISTS;
    default:
        return PT_STATUS_ERROR;
    }

    if (pte_encode_present(new_fpn, pte_decode_perm(old), &val) != 0)
        return PT_STATUS_ERROR;                    /* FPN tràn */
    node = malloc(sizeof(*node));                /* cấp TRƯỚC khi commit */
    if (node == NULL)
        return PT_STATUS_ERROR;

    /* commit */
    *w.pte = val;
    pgn = mm64_va_to_pgn((addr_t)MM64_VA_PAGE_BASE(va));
    pt_fifo_forget(mm, pgn);
    node->pgn = pgn;
    node->pg_next = mm->fifo_pgn;
    mm->fifo_pgn = node;
    return PT_STATUS_OK;
}




__attribute__((weak)) int mm_reclaim_global_user(struct pcb_t* caller, int need)
{
    (void)caller; (void)need; return 0;
}

__attribute__((weak)) int mm_reclaim_kernel_cache(struct pcb_t* caller, int need)
{
    (void)caller; (void)need; return 0;
}

__attribute__((weak)) int mm_swap_active_user(struct pcb_t* caller, int need)
{
    (void)caller; (void)need; return 0;
}

#define MM64_ALLOC_OOM ((addr_t)-3000)


static void pt_frame_list_release(struct memphy_struct* mram,
    struct framephy_struct* lst)
{
    while (lst != NULL) {
        struct framephy_struct* nx = lst->fp_next;
        MEMPHY_put_freefp(mram, lst->fpn);
        free(lst);
        lst = nx;
    }
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 * @caller    : caller
 * @req_pgnum : request page num
 * @frm_lst   : frame list
 */

addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst)
{
    int (* const stages[])(struct pcb_t*, int) = {
    mm_reclaim_global_user, mm_reclaim_kernel_cache, mm_swap_active_user
    };
    const int nstages = (int)(sizeof(stages) / sizeof(stages[0]));
    struct memphy_struct* mram;
    struct framephy_struct* head = NULL, ** tail = &head;
    int got = 0, stage = 0;
    int just_reclaimed = 0;   /* hook vừa báo có frame mà vẫn lấy không được -> sang tầng sau */

    if (frm_lst == NULL)
        return (addr_t)(int)PT_STATUS_ERROR;
    *frm_lst = NULL;
    if (caller == NULL || caller->krnl == NULL || caller->krnl->mram == NULL ||
        req_pgnum < 0)
        return (addr_t)(int)PT_STATUS_ERROR;
    if (req_pgnum == 0)
        return 0;

    mram = caller->krnl->mram;
    if ((uint64_t)req_pgnum > (uint64_t)mram->maxsz / MM64_PAGE_SIZE)
        return MM64_ALLOC_OOM;                      /* không bao giờ đủ */

    while (got < req_pgnum) {
        addr_t fpn;
        struct framephy_struct* nd;

        if (MEMPHY_get_freefp(mram, &fpn) != 0) {
            int freed;

            if (just_reclaimed)
                stage++;                                /* hook báo sai: không treo vòng lặp */
            if (stage >= nstages) {
                pt_frame_list_release(mram, head);
                return MM64_ALLOC_OOM;
            }
            freed = stages[stage](caller, req_pgnum - got);
            just_reclaimed = (freed > 0);
            if (!just_reclaimed)
                stage++;                                /* tầng này cạn -> tầng sau */
            continue;
        }
        just_reclaimed = 0;

        nd = malloc(sizeof(*nd));
        if (nd == NULL) {
            MEMPHY_put_freefp(mram, fpn);
            pt_frame_list_release(mram, head);
            return (addr_t)(int)PT_STATUS_ERROR;
        }
        nd->fpn = fpn;
        nd->fp_next = NULL;
        nd->owner = caller->mm;
        *tail = nd;
        tail = &nd->fp_next;
        got++;
    }

    *frm_lst = head;
    return 0;
}
/*
 * pt_vm_map - map a range of page at aligned address
 * @caller    : caller
 * @mm        : mm_struct
 * @astart    : vm area start
 * @aend      : vm area end
 * @mapstart  : start mapping point
 * @incpgnum  : number of mapped page
 * @ret_rg    : returned region
 * @perm      : permission for mapping pages
 * @track_fifo: track fifo list or not
 */
static addr_t pt_vm_map(struct pcb_t* caller, struct mm_struct* mm,
    addr_t astart, addr_t aend, addr_t mapstart,
    int incpgnum, struct vm_rg_struct* ret_rg,
    uint64_t perm, int track_fifo)
{
    struct framephy_struct* frm_lst = NULL, * fp;
    uint64_t len;
    addr_t ret;
    int rc;

    if (caller == NULL || caller->krnl == NULL || mm == NULL || incpgnum < 0)
        return (addr_t)(int)PT_STATUS_ERROR;

    /* 1. nằm trong area */
    len = (uint64_t)incpgnum * MM64_PAGE_SIZE;
    if ((uint64_t)mapstart < (uint64_t)astart ||
        (uint64_t)mapstart >(uint64_t)aend ||
        len > (uint64_t)aend - (uint64_t)mapstart)
        return (addr_t)(int)PT_STATUS_INVALID_VA;

    /* 2. vùng đích hợp lệ và trống */
    rc = pt_range_check_empty((mm64_table_t*)mm->pgd, mapstart,
        (uint64_t)incpgnum, NULL);
    if (rc != PT_STATUS_OK)
        return (addr_t)(int)rc;
    if (incpgnum == 0) {
        if (ret_rg != NULL)
            ret_rg->rg_start = ret_rg->rg_end = mapstart;
        return 0;
    }

    /* 3. frame */
    ret = alloc_pages_range(caller, incpgnum, &frm_lst);
    if (ret != 0)
        return ret;                                 /* -3000 hoặc lỗi khác, không giữ frame nào */
    for (fp = frm_lst; fp != NULL; fp = fp->fp_next)
        fp->owner = mm;

    /* 4. map */
    rc = pt_map_range(mm, mapstart, (uint64_t)incpgnum, frm_lst, perm, track_fifo);
    if (rc != PT_STATUS_OK) {
        pt_frame_list_release(caller->krnl->mram, frm_lst);   /* trả frame + free node */
        return (addr_t)(int)rc;
    }

    /* 5. thành công */
    if (ret_rg != NULL) {
        ret_rg->rg_start = mapstart;
        ret_rg->rg_end = (addr_t)((uint64_t)mapstart + len);
    }
    while (frm_lst != NULL) {
        fp = frm_lst->fp_next;
        free(frm_lst);
        frm_lst = fp;
    }
    return 0;
}

/*
 * vm_map_ram - do the mapping all vm are to ram storage device
 * @caller    : caller
 * @astart    : vm area start
 * @aend      : vm area end
 * @mapstart  : start mapping point
 * @incpgnum  : number of mapped page
 * @ret_rg    : returned region
 */
addr_t vm_map_ram(struct pcb_t *caller, addr_t astart, addr_t aend, addr_t mapstart, int incpgnum, struct vm_rg_struct *ret_rg)
{
  struct framephy_struct *frm_lst = NULL;
  addr_t ret_alloc = 0;

  /*@bksysnet: author provides a feasible solution of getting frames
   *FATAL logic in here, wrong behaviour if we have not enough page
   *i.e. we request 1000 frames meanwhile our RAM has size of 3 frames
   *Don't try to perform that case in this simple work, it will result
   *in endless procedure of swap-off to get frame and we have not provide
   *duplicate control mechanism, keep it simple
   */
  if (caller == NULL)
      return (addr_t)(int)PT_STATUS_ERROR;
  return pt_vm_map(caller, caller->mm, astart, aend, mapstart, incpgnum, ret_rg,
      MM64_PTE_DEFAULT_USER & MM64_PTE_PERM_MASK, 1);

  return 0;
}


/*
 * vm_map_kernel - do the mapping all vm are to kernel storage device
 * @caller    : caller
 * @astart    : vm area start
 * @aend      : vm area end
 * @mapstart  : start mapping point
 * @incpgnum  : number of mapped page
 * @ret_rg    : returned region
 */
addr_t vm_map_kernel(struct pcb_t* caller, addr_t astart, addr_t aend, addr_t mapstart, int incpgnum, struct vm_rg_struct* ret_rg)
{
    if (caller == NULL || caller->krnl == NULL)
        return (addr_t)(int)PT_STATUS_ERROR;
    return pt_vm_map(caller, caller->krnl->mm, astart, aend, mapstart, incpgnum,
        ret_rg, MM64_PTE_DEFAULT_KERNEL & MM64_PTE_PERM_MASK, 0);
}

/* Swap copy content page from source frame to destination frame
 * @mpsrc  : source memphy
 * @srcfpn : source physical page number (FPN)
 * @mpdst  : destination memphy
 * @dstfpn : destination physical page number (FPN)
 **/
int __swap_cp_page(struct memphy_struct *mpsrc, addr_t srcfpn,
                   struct memphy_struct *mpdst, addr_t dstfpn)
{
  addr_t addrsrc;
  addr_t addrdst;
  addr_t cellidx;

  if (mpsrc == NULL || mpdst == NULL
      || mpsrc->storage == NULL || mpdst->storage == NULL
      || mpsrc->maxsz < PAGING64_PAGESZ
      || mpdst->maxsz < PAGING64_PAGESZ)
    return -1;

  /* Check frame bounds before multiplication so an invalid FPN cannot wrap. */
  if (srcfpn >= (addr_t)mpsrc->maxsz / PAGING64_PAGESZ
      || dstfpn >= (addr_t)mpdst->maxsz / PAGING64_PAGESZ)
    return -1;

  addrsrc = srcfpn * PAGING64_PAGESZ;
  addrdst = dstfpn * PAGING64_PAGESZ;

  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    BYTE data;

    if (MEMPHY_read(mpsrc, addrsrc + cellidx, &data) != 0)
      return -1;
    if (MEMPHY_write(mpdst, addrdst + cellidx, data) != 0)
      return -1;
  }

  return 0;
}

/*
 *Initialize a empty Memory Management instance
 * @mm:     self mm
 * @caller: mm owner
 */
int init_mm(struct mm_struct *mm, struct pcb_t *caller)
{
  /* P1: tạo PGD root rỗng (mm->pgd), trước khi cấp VMA để lỗi không rò vma0 */
  if (pt_init_root(mm) != PT_STATUS_OK)
      return -1;

  struct vm_area_struct* vma0 = malloc(sizeof(struct vm_area_struct));


  /* By default the owner comes with at least one vma */
  vma0->vm_id = 0;
  vma0->vm_start = 0;
  vma0->vm_end = vma0->vm_start;
  vma0->sbrk = vma0->vm_start;
  vma0->vm_freerg_list = NULL;
  struct vm_rg_struct *first_rg = init_vm_rg(vma0->vm_start, vma0->vm_end);
  enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);

  /* TODO update VMA0 next */
  vma0->vm_next = NULL;   /* moi co 1 vma, chua co vma ke tiep */

  /* Point vma owner backward */
  vma0->vm_mm = mm;

  /* TODO: update mmap */
  mm->mmap = vma0;
  /* symrgtbl la mang co san trong struct, cho ve 0 het de active=0 */
  for (int i = 0; i < PAGING_MAX_SYMTBL_SZ; i++)
  {
    mm->symrgtbl[i].vmaid    = 0;
    mm->symrgtbl[i].rg_start = 0;
    mm->symrgtbl[i].rg_end   = 0;
    mm->symrgtbl[i].active   = 0;
    mm->symrgtbl[i].rg_next  = NULL;
  }

  return 0;
}

struct vm_rg_struct *init_vm_rg(addr_t rg_start, addr_t rg_end)
{
  struct vm_rg_struct *rgnode = malloc(sizeof(struct vm_rg_struct));

  rgnode->vmaid = 0;
  rgnode->rg_start = rg_start;
  rgnode->rg_end = rg_end;
  rgnode->active = 0;        /* node vung free, chua cap cho ai */
  rgnode->rg_next = NULL;

  return rgnode;
}

int enlist_vm_rg_node(struct vm_rg_struct **rglist, struct vm_rg_struct *rgnode)
{
  rgnode->rg_next = *rglist;
  *rglist = rgnode;

  return 0;
}

int enlist_pgn_node(struct pgn_t **plist, addr_t pgn)
{
  struct pgn_t *pnode = malloc(sizeof(struct pgn_t));

  pnode->pgn = pgn;
  pnode->pg_next = *plist;
  *plist = pnode;

  return 0;
}

int print_list_fp(struct framephy_struct *ifp)
{
  struct framephy_struct *fp = ifp;

  printf("print_list_fp: ");
  if (fp == NULL) { printf("NULL list\n"); return -1;}
  printf("\n");
  while (fp != NULL)
  {
    printf("fp[" FORMAT_ADDR "]\n", fp->fpn);
    fp = fp->fp_next;
  }
  printf("\n");
  return 0;
}

int print_list_rg(struct vm_rg_struct *irg)
{
  struct vm_rg_struct *rg = irg;

  printf("print_list_rg: ");
  if (rg == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (rg != NULL)
  {
    /* in them vmaid + active de debug reuse/active/free */
    printf("rg[" FORMAT_ADDR "->"  FORMAT_ADDR "] vmaid=%d %s\n",
           rg->rg_start, rg->rg_end, rg->vmaid,
           rg->active ? "ACTIVE" : "free");
    rg = rg->rg_next;
  }
  printf("\n");
  return 0;
}

int print_list_vma(struct vm_area_struct *ivma)
{
  struct vm_area_struct *vma = ivma;

  printf("print_list_vma: ");
  if (vma == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (vma != NULL)
  {
    /* in them id + sbrk (ranh gioi vung dang dung) de debug */
    printf("va[" FORMAT_ADDR "->" FORMAT_ADDR "] id=%lu sbrk=" FORMAT_ADDR "\n",
           vma->vm_start, vma->vm_end,
           (unsigned long)vma->vm_id, vma->sbrk);
    vma = vma->vm_next;
  }
  printf("\n");
  return 0;
}

int print_list_pgn(struct pgn_t *ip)
{
  printf("print_list_pgn: ");
  if (ip == NULL) { printf("NULL list\n"); return -1; }
  printf("\n");
  while (ip != NULL)
  {
    printf("va[" FORMAT_ADDR "]-\n", ip->pgn);
    ip = ip->pg_next;
  }
  printf("\n");
  return 0;
}

int print_pgtbl(struct pcb_t *caller, addr_t start, addr_t end)
{
//addr_t pgn = ...
//addr_t pgit = ...
//struct mm_struct *mm = ...
  addr_t pgd=0;
  addr_t p4d=0;
  addr_t pud=0;
  addr_t pmd=0;
  addr_t pt=0;

  get_pd_from_address(start, &pgd, &p4d, &pud, &pmd, &pt);

  /* TODO traverse the page map and dump the page directory entries */

  return 0;
}

#endif  //def MM64
