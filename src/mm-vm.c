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

int __mm_swap_page(struct pcb_t *caller, addr_t vicfpn , addr_t swpfpn)
{
    __swap_cp_page(caller->krnl->mram, vicfpn, caller->krnl->active_mswp, swpfpn);
    return 0;
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
 */
int validate_overlap_vm_area(struct pcb_t *caller, int vmaid, addr_t vmastart, addr_t vmaend)
{
  struct vm_area_struct *vma = caller->mm->mmap;

  /* TODO validate the planned memory area is not overlapped */
  if (vmastart >= vmaend)
  {
    return -1;
  }

  if (vma == NULL)
  {
    return -1;
  }

  /* TODO validate the planned memory area is not overlapped */

  struct vm_area_struct *cur_area = get_vma_by_num(caller->mm, vmaid);
  if (cur_area == NULL)
  {
    return -1;
  }

  while (vma != NULL)
  {
    if (vma != cur_area && OVERLAP(cur_area->vm_start, cur_area->vm_end, vma->vm_start, vma->vm_end))
    {
      return -1;
    }
    vma = vma->vm_next;
  }
  /* End TODO*/

  return 0;
}

/*inc_vma_limit - increase vm area limits to reserve space for new variable
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@inc_sz: increment size
 *
 */
int inc_vma_limit(struct pcb_t *caller, int vmaid, addr_t inc_sz)
{
  /* TOTO with new address scheme, the size need tobe aligned
   *      the raw inc_sz maybe not fit pagesize
   */
  /* lam tron inc_sz len boi so cua page */
  addr_t inc_amt    = PAGING64_PAGE_ALIGNSZ(inc_sz);
  int    incnumpage = inc_amt / PAGING64_PAGESZ;

  /* TODO: Obtain the new vm area based on vmaid */
  /* lay vung moi tai break */
  struct vm_rg_struct *area = get_vm_area_node_at_brk(caller, vmaid, inc_amt);
  if (area == NULL)
    return -1;

  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (cur_vma == NULL) { free(area); return -1; }

  /*
   * TODO Validate overlap of obtained region
   *if (validate_overlap_vm_area(caller, vmaid, area->rg_start, area->rg_end) < 0)
   *   return -1;  //Overlap and failed allocation
   */

  if (validate_overlap_vm_area(caller, vmaid, area->rg_start, area->rg_end) < 0)
  {
    free(area);
    return -1;  /* overlap -> failed allocation */
  }

  /* goi P1 map frame; vm_map_ram tra addr_t, sentinel am khi loi */
  addr_t old_end = cur_vma->vm_end;
  struct vm_rg_struct *mapped = malloc(sizeof(struct vm_rg_struct));
  if (mapped == NULL) { free(area); return -1; }

  if ((long)vm_map_ram(caller, area->rg_start, area->rg_end,
                       old_end, incnumpage, mapped) < 0)
  {
    free(area); free(mapped);
    return -1;
  }

  cur_vma->vm_end = old_end + inc_amt;
  cur_vma->sbrk   = area->rg_end;

  free(area); free(mapped);
  return 0;
}

// #endif
