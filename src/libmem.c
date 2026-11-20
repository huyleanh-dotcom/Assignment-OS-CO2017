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
static pthread_mutex_t kmm_lock = PTHREAD_MUTEX_INITIALIZER;

/*enlist_vm_freerg_list - add new rg to freerg_list
 *@mm: memory region
 *@rg_elmt: new region
 *
 */
int enlist_vm_freerg_list(struct mm_struct *mm, struct vm_rg_struct *rg_elmt)
{
  if (mm == NULL || mm->mmap == NULL || rg_elmt == NULL)
    return -1;
  if (rg_elmt->rg_start >= rg_elmt->rg_end)
    return -1;

  /* coalesce: neu vung free lien ke vung co san thi gop lai */
  struct vm_rg_struct *it = mm->mmap->vm_freerg_list;
  while (it != NULL) {
    if (it->rg_end == rg_elmt->rg_start) { it->rg_end = rg_elmt->rg_end; free(rg_elmt); return 0; }
    if (rg_elmt->rg_end == it->rg_start) { it->rg_start = rg_elmt->rg_start; free(rg_elmt); return 0; }
    it = it->rg_next;
  }

  /* Enlist the new region */
  rg_elmt->rg_next = mm->mmap->vm_freerg_list;
  mm->mmap->vm_freerg_list = rg_elmt;

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

/* tra 0 neu symrgtbl[rgid] la vung active hop le; nguoc lai -1.
 * P3 (READ/WRITE) phai goi truoc khi cham frame. */
int vm_validate_active_region(struct pcb_t *caller, int rgid)
{
  if (caller == NULL) return -1;
  struct vm_rg_struct *rg = get_symrg_byid(caller->mm, rgid);
  if (rg == NULL || rg->active != 1) return -1;
  if (rg->rg_start >= rg->rg_end) return -1;
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
  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ || size == 0)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  struct vm_rg_struct rgnode;
  struct vm_area_struct *cur_vma = get_vma_by_num(caller->mm, vmaid);
  if (cur_vma == NULL)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  if (get_free_vmrg_area(caller, vmaid, size, &rgnode) == 0)
  {
    caller->mm->symrgtbl[rgid].vmaid    = vmaid;
    caller->mm->symrgtbl[rgid].rg_start = rgnode.rg_start;
    caller->mm->symrgtbl[rgid].rg_end = rgnode.rg_end;
    caller->mm->symrgtbl[rgid].active   = 1;

    *alloc_addr = rgnode.rg_start;

    pthread_mutex_unlock(&mmvm_lock);
    return 0;
  }

  /* TODO get_free_vmrg_area FAILED handle the region management (Fig.6)*/
  /* khong co vung free vua -> xin them qua syscall; nho break cu */
  addr_t old_sbrk = cur_vma->sbrk;

  /* TODO INCREASE THE LIMIT
   * SYSCALL 1 sys_memmap
   */
  /* goi SYSCALL 17 sys_memmap voi op SYSMEM_INC_OP */
  struct sc_regs regs;
  regs.a1 = SYSMEM_INC_OP;
  regs.a2 = vmaid;
  regs.a3 = size;

  if (_syscall(caller->krnl, caller->pid, 17, &regs) != 0)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;   /* rollback: syscall loi, khong ghi symrgtbl */
  }

  /*Successful increase limit */
  caller->mm->symrgtbl[rgid].vmaid    = vmaid;
  caller->mm->symrgtbl[rgid].rg_start = old_sbrk;
  caller->mm->symrgtbl[rgid].rg_end = old_sbrk + size;
  caller->mm->symrgtbl[rgid].active   = 1;

  *alloc_addr = old_sbrk;

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
int __free(struct pcb_t *caller, int vmaid, int rgid)
{
  pthread_mutex_lock(&mmvm_lock);

  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ)   /* '>' -> '>=' */
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  /* TODO: Manage the collect freed region to freerg_list */
  /* quy uoc: vung chua dung co rg_start==rg_end==0 */
  struct vm_rg_struct *rgnode = get_symrg_byid(caller->mm, rgid);

  if (rgnode == NULL || rgnode->active != 1)   /* chi free vung dang active */
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;   /* khong co / da free -> tu choi (chong double free) */
  }
  struct vm_rg_struct *freerg_node = malloc(sizeof(struct vm_rg_struct));
  if (freerg_node == NULL)
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }
  freerg_node->vmaid = vmaid;
  freerg_node->rg_start = rgnode->rg_start;
  freerg_node->rg_end = rgnode->rg_end;
  freerg_node->active = 0;
  freerg_node->rg_next = NULL;

  rgnode->active = 0;                      /* danh dau slot free */
  rgnode->rg_start = rgnode->rg_end = 0;
  rgnode->rg_next = NULL;

  /*enlist the obsoleted memory region */
  enlist_vm_freerg_list(caller->mm, freerg_node);

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
  proc->regs[reg_index] = addr;

#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
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
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif
  return 0;//val;
}

/*pg_getpage - get the page in ram
 *@mm: memory region
 *@pagenum: PGN
 *@framenum: return FPN
 *@caller: caller
 *
 */
int pg_getpage(struct mm_struct *mm, int pgn, int *fpn, struct pcb_t *caller)
{

  uint64_t pte = pte_get_entry(caller->mm, pgn, caller);

  if (!PAGING_PAGE_PRESENT(pte))
  { /* Page is not online, make it actively living */
    addr_t vicpgn;
    addr_t vicpte;
    addr_t vicfpn, swpfpn;
    struct sc_regs regs;

    /* TODO Initialize the target frame storing our variable */
//  addr_t tgtfpn 

    /* TODO: Play with your paging theory here */
    /* Find victim page */
    if (find_victim_page(caller->mm, &vicpgn) == -1)
    {
      return -1;
    }

    /* Get free frame in MEMSWP */
    if (MEMPHY_get_freefp(caller->krnl->active_mswp, &swpfpn) == -1)
    {
      return -1;
    }

    /* TODO: Implement swap frame from MEMRAM to MEMSWP and vice versa*/

    /* TODO copy victim frame to swap 
     * SWP(vicfpn <--> swpfpn)
     * SYSCALL 1 sys_memmap
     */


    /* Update page table */
    //pte_set_swap(...);

    /* Update its online status of the target page */
    //pte_set_fpn(...);

    enlist_pgn_node(&caller->mm->fifo_pgn, pgn);
  }

  *fpn = PAGING_FPN(pte_get_entry(caller->mm,pgn, caller));

  return 0;
}

/*pg_getval - read value at given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_getval(struct mm_struct *mm, int addr, BYTE *data, struct pcb_t *caller)
{
  int pgn = PAGING_PGN(addr);
//int off = ...
  int fpn;

  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */

//int phyaddr = ...

  /* TODO 
   *  MEMPHY_read(caller->krnl->mram, phyaddr, data);
   *  MEMPHY READ 
   *  SYSCALL 17 sys_memmap with SYSMEM_IO_READ
   */

  return 0;
}

/*pg_setval - write value to given offset
 *@mm: memory region
 *@addr: virtual address to acess
 *@value: value
 *
 */
int pg_setval(struct mm_struct *mm, int addr, BYTE value, struct pcb_t *caller)
{
  int pgn = PAGING_PGN(addr);
//int off = ...
  int fpn;

  /* Get the page to MEMRAM, swap from MEMSWAP if needed */
  if (pg_getpage(mm, pgn, &fpn, caller) != 0)
    return -1; /* invalid page access */


  /* TODO 
   *  MEMPHY_write(caller->krnl->mram, phyaddr, value);
   *  MEMPHY WRITE with SYSMEM_IO_WRITE 
   * SYSCALL 17 sys_memmap
   */

  return 0;
}

/*__read - read value in region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __read(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  struct vm_rg_struct *currg = get_symrg_byid(caller->mm, rgid);

//struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);

  /* TODO Invalid memory identify */

  pg_getval(caller->krnl->mm, currg->rg_start + offset, data, caller);

  return 0;
}

/*libread - PAGING-based read a region memory */
int libread(
    struct pcb_t *proc, // Process executing the instruction
    uint32_t source,    // Index of source register
    addr_t offset,    // Source address = [source] + [offset]
    uint32_t* destination)
{
  BYTE data;
printf("%s:%d\n",__func__,__LINE__);
  int val = __read(proc, 0, source, offset, &data);

  *destination = data;
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}

/*__write - write a region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@offset: offset to acess in memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: allocated size
 *
 */
int __write(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  pthread_mutex_lock(&mmvm_lock);
  struct vm_rg_struct *currg = get_symrg_byid(caller->krnl->mm, rgid);

  struct vm_area_struct *cur_vma = get_vma_by_num(caller->krnl->mm, vmaid);

  if (currg == NULL || cur_vma == NULL) /* Invalid memory identify */
  {
    pthread_mutex_unlock(&mmvm_lock);
    return -1;
  }

  pg_setval(caller->krnl->mm, currg->rg_start + offset, value, caller);

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}

/*libwrite - PAGING-based write a region memory */
int libwrite(
    struct pcb_t *proc,   // Process executing the instruction
    BYTE data,            // Data to be wrttien into memory
    uint32_t destination, // Index of destination register
    addr_t offset)
{
  int val = __write(proc, 0, destination, offset, data);
  if (val == -1)
  {
    return -1;
  }
#ifdef IODUMP
  /* TODO dump IO content (if needed) */
#ifdef PAGETBL_DUMP
  print_pgtbl(proc, 0, -1); // print max TBL
#endif
#endif

  return val;
}


/*libkmem_malloc- alloc region memory in kmem
 *@caller: caller
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@size: memory size
 */

/* kernel heap cap phat lien tuc, dung chung 1 con tro break cho ca kmalloc va pool.
 * base lay tu kernel_base (os.c dat theo config), khoi tao lan dau khi can. */
extern unsigned long kernel_base;
static addr_t k_brk = 0;

/* cap size byte tu kernel break, tra dia chi dau. phai dang giu kmm_lock khi goi */
static addr_t kmem_bump(addr_t size)
{
  if (k_brk == 0) k_brk = (addr_t)kernel_base;
  addr_t start = k_brk;
  k_brk += PAGING64_PAGE_ALIGNSZ(size);
  return start;
}

/* them 1 chunk slot co dinh vao free list cua pool; CALLER GIU kmm_lock */
int kmem_pool_grow(struct kcache_pool_struct *pool)
{
  if (pool == NULL || pool->size <= 0) return -1;
  int n = pool->slots_per_chunk > 0 ? pool->slots_per_chunk : 8;
  addr_t chunk = kmem_bump((addr_t)pool->size * n);
  for (int i = 0; i < n; i++) {
    struct kmem_slot_node *node = malloc(sizeof(struct kmem_slot_node));
    if (node == NULL) return -1;
    node->kva = chunk + (addr_t)i * pool->size;
    node->next = pool->free_list;
    pool->free_list = node;
  }
  return 0;
}

/* lay 1 slot trong (grow neu het); tra kernel VA hoac -1 */
addr_t kmem_pool_alloc_slot(struct kcache_pool_struct *pool)
{
  if (pool == NULL) return (addr_t)-1;
  pthread_mutex_lock(&kmm_lock);
  if (pool->free_list == NULL && kmem_pool_grow(pool) != 0) {
    pthread_mutex_unlock(&kmm_lock);
    return (addr_t)-1;
  }
  struct kmem_slot_node *node = pool->free_list;
  pool->free_list = node->next;
  addr_t kva = node->kva;
  free(node);
  pthread_mutex_unlock(&kmm_lock);
  return kva;
}

/* tra 1 slot ve free list; chong double-free */
int kmem_pool_free_slot(struct kcache_pool_struct *pool, addr_t kva)
{
  if (pool == NULL) return -1;
  pthread_mutex_lock(&kmm_lock);
  for (struct kmem_slot_node *it = pool->free_list; it != NULL; it = it->next)
    if (it->kva == kva) { pthread_mutex_unlock(&kmm_lock); return -1; }
  struct kmem_slot_node *node = malloc(sizeof(struct kmem_slot_node));
  if (node == NULL) { pthread_mutex_unlock(&kmm_lock); return -1; }
  node->kva = kva; node->next = pool->free_list; pool->free_list = node;
  pthread_mutex_unlock(&kmm_lock);
  return 0;
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
  caller->regs[reg_index] = addr;

  /*
   * TODO: provide OS kmem allocation validation
   */
#ifdef IODUMP
#ifdef PAGETBL_DUMP
  print_pgtbl(caller, 0, -1);
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
  if (caller == NULL || caller->krnl == NULL || caller->krnl->mm == NULL)
    return (addr_t)-1;
  if (rgid < 0 || rgid >= PAGING_MAX_SYMTBL_SZ || size == 0)
    return (addr_t)-1;

  pthread_mutex_lock(&kmm_lock);

  addr_t start = kmem_bump(size);   /* break dung chung voi slab pool */

  /* ghi vung vao bang symbol cua kernel mm */
  struct mm_struct *kmm = caller->krnl->mm;
  kmm->symrgtbl[rgid].vmaid    = vmaid;
  kmm->symrgtbl[rgid].rg_start = start;
  kmm->symrgtbl[rgid].rg_end   = start + size;
  kmm->symrgtbl[rgid].active   = 1;   /* danh dau vung dang dung */

  *alloc_addr = start;

  pthread_mutex_unlock(&kmm_lock);
  return 0;

}

addr_t __kmem_cache_alloc(struct pcb_t *caller, int vmaid, int rgid, int cache_pool_id, addr_t *alloc_addr)
{
  /* lay 1 slot san co trong pool, het slot thi pool tu xin them tu kernel heap.
   * do slot co kich thuoc co dinh nen it bi phan manh hon goi __kmalloc truc tiep */
  if (caller == NULL || caller->krnl == NULL || caller->krnl->kcpooltbl == NULL)
    return (addr_t)-1;
  if (cache_pool_id < 0)
    return (addr_t)-1;

  struct kcache_pool_struct *pool = &caller->krnl->kcpooltbl[cache_pool_id];
  if (pool->size <= 0)
    return (addr_t)-1;

  addr_t kva = kmem_pool_alloc_slot(pool);
  if (kva == (addr_t)-1)
    return (addr_t)-1;

  /* ghi vung vao bang symbol cua kernel mm giong __kmalloc */
  struct mm_struct *kmm = caller->krnl->mm;
  if (rgid >= 0 && rgid < PAGING_MAX_SYMTBL_SZ && kmm != NULL) {
    pthread_mutex_lock(&kmm_lock);
    kmm->symrgtbl[rgid].vmaid    = vmaid;
    kmm->symrgtbl[rgid].rg_start = kva;
    kmm->symrgtbl[rgid].rg_end   = kva + pool->size;
    kmm->symrgtbl[rgid].active   = 1;
    pthread_mutex_unlock(&kmm_lock);
  }

  if (alloc_addr != NULL)
    *alloc_addr = kva;
  return 0;
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

  return 0;
}


int libkmem_copy_from_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_user_mem(...)
  //__write_kernel_mem(...);

  return 0;
}

int libkmem_copy_to_user(struct pcb_t *caller, uint32_t source, uint32_t destination, uint32_t offset, uint32_t size)
{
  /* TODO: provide OS level management kmem
   */
  /*
   * TODO: Map kernel address range
   */
  //__read_kernel_mem(...)
  //__write_user_mem(...);

  return 1;
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

  return 0;
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

  return 0;
}

/*__read_user_mem - read value in user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __read_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE *data)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy

   return 0;
}


/*__write_user_mem - write a user region memory
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@rgid: memory region ID (used to identify variable in symbole table)
 *@offset: offset to acess in memory region
 *@value: data value
 */
int __write_user_mem(struct pcb_t *caller, int vmaid, int rgid, addr_t offset, BYTE value)
{
  /* TODO: provide OS level management user memory access */
  //krnl->pgd ... or krnl->krnl_pgd  ... based on kmem implementation strategy

  return 0;
}


/*free_pcb_memphy - collect all memphy of pcb
 *@caller: caller
 *@vmaid: ID vm area to alloc memory region
 *@incpgnum: number of page
 */
int free_pcb_memph(struct pcb_t *caller)
{
  pthread_mutex_lock(&mmvm_lock);
  int pagenum, fpn;
  uint32_t pte;

  for (pagenum = 0; pagenum < PAGING_MAX_PGN; pagenum++)
  {
    pte = caller->mm->pgd[pagenum];

    if (PAGING_PAGE_PRESENT(pte))
    {
      fpn = PAGING_FPN(pte);
      MEMPHY_put_freefp(caller->krnl->mram, fpn);
    }
    else
    {
      fpn = PAGING_SWP(pte);
      MEMPHY_put_freefp(caller->krnl->active_mswp, fpn);
    }
  }

  pthread_mutex_unlock(&mmvm_lock);
  return 0;
}


/*find_victim_page - find victim page
 *@caller: caller
 *@pgn: return page number
 *
 */
int find_victim_page(struct mm_struct *mm, addr_t *retpgn)
{
  struct pgn_t *pg = mm->fifo_pgn;

  /* TODO: Implement the theorical mechanism to find the victim page */
  if (!pg)
  {
    return -1;
  }
  struct pgn_t *prev = NULL;
  while (pg->pg_next)
  {
    prev = pg;
    pg = pg->pg_next;
  }
  *retpgn = pg->pgn;
  prev->pg_next = NULL;

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
