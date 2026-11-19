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

/*  * pt_table_alloc - Allocate a new page table for a given level
 * @level: the level of the page table (0 for PGD, 1 for P4D, etc.)
 * Return: pointer to the allocated page table, or NULL on failure
 */
static mm64_table_t* pt_table_alloc(unsigned level)
{
    mm64_table_t* t;

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

/* pte_rollback_created_branches - recover branches */
static int pt_rollback_created_branches(struct pt_walk* w)
{
    int l;

    for (l = MM64_PT_LEVELS - 1; l >= 1; l--) {
        mm64_pde_t* pe;

        if (!(w->created & (1u << l)) || w->tbl[l] == NULL)
            continue;

        pe = &w->tbl[l - 1]->entries[w->idx[l - 1]];
        if (pde_count(*pe) != 0)
            return -1;

        pt_table_free(w->tbl[l], (unsigned)l);
        w->tbl[l] = NULL;
        w->created &= ~(1u << l);
        if (l == (int)MM64_LVL_PT)
            w->pte = NULL;

        *pe = 0;
        pt_count_dec(w, (unsigned)(l - 1));
    }
    return 0;
}

/* pt_prune_empty - prune empty page tables after a PTE is cleared
 * @w: the pt_walk structure after a PTE has been cleared
 */
static void pt_prune_empty(struct pt_walk* w)
{
    int l;

    for (l = MM64_PT_LEVELS - 1; l >= 1; l--) {
        mm64_pde_t* pe;

        if (w->tbl[l] == NULL)
            continue;                   

        pe = &w->tbl[l - 1]->entries[w->idx[l - 1]];
        if (pde_count(*pe) != 0)
            break;                      

        pt_table_free(w->tbl[l], (unsigned)l);
        w->tbl[l] = NULL;
        w->created &= ~(1u << l);
        if (l == (int)MM64_LVL_PT)
            w->pte = NULL;

        *pe = 0;                      
        pt_count_dec(w, (unsigned)(l - 1));
    }
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


/*
 * pte_set_swap - Set PTE entry for swapped page
 * @pte    : target page table entry (PTE)
 * @swptyp : swap type
 * @swpoff : swap offset
 */
int pte_set_swap(struct pcb_t *caller, addr_t pgn, int swptyp, addr_t swpoff)
{
  struct krnl_t *krnl = caller->krnl;

  addr_t *pte;
  addr_t pgd=0;
  addr_t p4d=0;
  addr_t pud=0;
  addr_t pmd=0;
  addr_t pt=0;
	
  // dummy pte alloc to avoid runtime error
  pte = malloc(sizeof(addr_t));
#ifdef MM64	
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
  return pte_set_entry(mm, pgn, val, caller);
#else
  pte = &krnl->mm->pgd[pgn];
#endif
	
  CLRBIT(*pte, PAGING_PTE_PRESENT_MASK);
  SETBIT(*pte, PAGING_PTE_SWAPPED_MASK);

  SETVAL(*pte, swptyp, PAGING_PTE_SWPTYP_MASK, PAGING_PTE_SWPTYP_LOBIT);
  SETVAL(*pte, swpoff, PAGING_PTE_SWPOFF_MASK, PAGING_PTE_SWPOFF_LOBIT);

  return 0;
}

/*
 * pte_set_fpn - Set PTE entry for on-line page
 * @pte   : target page table entry (PTE)
 * @fpn   : frame page number (FPN)
 * @caller: caller
 */
int pte_set_fpn(struct mm_struct *mm, addr_t pgn, addr_t fpn, struct pcb_t *caller)
{
  struct krnl_t *krnl = caller->krnl;

  addr_t *pte = NULL;
  addr_t pgd=0;
  addr_t p4d=0;
  addr_t pud=0;
  addr_t pmd=0;
  addr_t pt=0;
	
  // dummy pte alloc to avoid runtime error
  pte = malloc(sizeof(addr_t));
#ifdef MM64	
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
#else
  pte = &krnl->mm->pgd[pgn];
#endif

  SETBIT(*pte, PAGING_PTE_PRESENT_MASK);
  CLRBIT(*pte, PAGING_PTE_SWAPPED_MASK);

  SETVAL(*pte, fpn, PAGING_PTE_FPN_MASK, PAGING_PTE_FPN_LOBIT);

  return 0;
}


/* Get PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @caller: caller
 * @ret    : page table entry
 **/
addr_t pte_get_entry(struct mm_struct *mm, addr_t pgn, struct pcb_t *caller)
{
  struct krnl_t *krnl = caller->krnl;

  addr_t *pte = NULL;
  addr_t pgd=0;
  addr_t p4d=0;
  addr_t pud=0;
  addr_t pmd=0;
  addr_t pt=0;
	
  // dummy pte alloc to avoid runtime error
  pte = malloc(sizeof(addr_t));
#ifdef MM64	
  struct pt_walk w;

  mm = pt_resolve_mm(mm, caller);
  if (mm == NULL || (uint64_t)pgn > MM64_PGN_MAX)
      return 0;

  if (pt_walk_internal((mm64_table_t*)mm->pgd, mm64_pgn_to_va(pgn), 0, &w)
      != PT_STATUS_OK)
      return 0;

  return (addr_t)*w.pte;
#endif

  return (addr_t) &pte;
}

/* Set PTE page table entry
 * @caller : caller
 * @pgn    : page number
 * @ret    : page table entry
 **/
int pte_set_entry(struct mm_struct *mm, addr_t pgn, uint64_t pte_val, struct pcb_t *caller)
{
  struct krnl_t *krnl = caller->krnl;

  addr_t *pte = NULL;
  addr_t pgd=0;
  addr_t p4d=0;
  addr_t pud=0;
  addr_t pmd=0;
  addr_t pt=0;
	
  // dummy pte alloc to avoid runtime error
  pte = malloc(sizeof(addr_t));
#ifdef MM64	
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
#else	
  pte = pte_val;
#endif

	return 0;
}

/*
 * vmap_pgd_memset - map a range of page at aligned address
 */
int vmap_pgd_memset(struct pcb_t *caller,           // process call
                    addr_t addr,                       // start address which is aligned to pagesz
                    int pgnum)                      // num of mapping page
{
  //int pgit = 0;
  //uint64_t pattern = 0xdeadbeef;

  /* TODO memset the page table with given pattern
   */

  return 0;
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
//struct framephy_struct *fpit;
//int pgit = 0;
//addr_t pgn;

  /* TODO: update the rg_end and rg_start of ret_rg 
  //ret_rg->rg_end =  ....
  //ret_rg->rg_start = ...
  //ret_rg->vmaid = ...
  */

  /* TODO map range of frame to address space
   *      [addr to addr + pgnum*PAGING_PAGESZ
   *      in page table caller->krnl->mm->pgd,
   *                    caller->krnl->mm->pud...
   *                    ...
   */

  /* Tracking for later page replacement activities (if needed)
   * Enqueue new usage page */
  //enlist_pgn_node(&caller->krnl->mm->fifo_pgn, pgn64 + pgit);

  return 0;
}

/*
 * alloc_pages_range - allocate req_pgnum of frame in ram
 * @caller    : caller
 * @req_pgnum : request page num
 * @frm_lst   : frame list
 */

addr_t alloc_pages_range(struct pcb_t *caller, int req_pgnum, struct framephy_struct **frm_lst)
{
  //addr_t fpn;
  //int pgit;
  //struct framephy_struct *newfp_str = NULL;
  //struct framephy_struct **cur = frm_lst;

  /* TODO: allocate the page 
  //caller-> ...
  //frm_lst-> ...
  */


/*
  for (pgit = 0; pgit < req_pgnum; pgit++)
  {
    // TODO: allocate the page 
    if (MEMPHY_get_freefp(caller->mram, &fpn) == 0)
    {
      newfp_str->fpn = fpn;
    }
    else
    { // TODO: ERROR CODE of obtaining somes but not enough frames
    }
  }
*/


  /* End TODO */

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
  ret_alloc = alloc_pages_range(caller, incpgnum, &frm_lst);

  if (ret_alloc < 0 && ret_alloc != -3000)
    return -1;

  /* Out of memory */
  if (ret_alloc == -3000)
  {
    return -1;
  }

  /* it leaves the case of memory is enough but half in ram, half in swap
   * do the swaping all to swapper to get the all in ram */
   vmap_page_range(caller, mapstart, incpgnum, frm_lst, ret_rg);

  return 0;
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
  int cellidx;
  addr_t addrsrc, addrdst;
#ifdef MM64
  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING64_PAGESZ + cellidx;
#else
  for (cellidx = 0; cellidx < PAGING64_PAGESZ; cellidx++)
  {
    addrsrc = srcfpn * PAGING64_PAGESZ + cellidx;
    addrdst = dstfpn * PAGING64_PAGESZ + cellidx;
#endif

    BYTE data;
    MEMPHY_read(mpsrc, addrsrc, &data);
    MEMPHY_write(mpdst, addrdst, data);
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
  struct vm_area_struct *vma0 = malloc(sizeof(struct vm_area_struct));

  /* TODO init page table directory */
   //mm->pgd = ...
   //mm->p4d = ...
   //mm->pud = ...
   //mm->pmd = ...
   //mm->pt = ...


  /* By default the owner comes with at least one vma */
  vma0->vm_id = 0;
  vma0->vm_start = 0;
  vma0->vm_end = vma0->vm_start;
  vma0->sbrk = vma0->vm_start;
  struct vm_rg_struct *first_rg = init_vm_rg(vma0->vm_start, vma0->vm_end);
  enlist_vm_rg_node(&vma0->vm_freerg_list, first_rg);

  /* TODO update VMA0 next */
  // vma0->next = ...

  /* Point vma owner backward */
  //vma0->vm_mm = mm; 

  /* TODO: update mmap */
  //mm->mmap = ...
  //mm->symrgtbl = ...
  //mm->kcpooltbl

  return 0;
}

struct vm_rg_struct *init_vm_rg(addr_t rg_start, addr_t rg_end)
{
  struct vm_rg_struct *rgnode = malloc(sizeof(struct vm_rg_struct));

  rgnode->rg_start = rg_start;
  rgnode->rg_end = rg_end;
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
    printf("rg[" FORMAT_ADDR "->"  FORMAT_ADDR "]\n", rg->rg_start, rg->rg_end);
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
    printf("va[" FORMAT_ADDR "->" FORMAT_ADDR "]\n", vma->vm_start, vma->vm_end);
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
