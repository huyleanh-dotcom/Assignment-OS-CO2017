/*
 * Copyright (C) 2025 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Sierra release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "common.h"

#define SYSMEM_MAP_OP 1
#define SYSMEM_INC_OP 2
#define SYSMEM_SWP_OP 3
#define SYSMEM_IO_READ 4
#define SYSMEM_IO_WRITE 5

extern struct vm_area_struct *get_vma_by_num(struct mm_struct *mm, int vmaid);
int liballoc(struct pcb_t *, addr_t, uint32_t);
int libfree(struct pcb_t *, uint32_t);
int libread(struct pcb_t*, uint32_t, addr_t, uint32_t*);
int libwrite(struct pcb_t*, BYTE, uint32_t, addr_t);
int libkmem_malloc(struct pcb_t*, uint32_t, uint32_t);
int libkmem_copy_from_user(struct pcb_t*, uint32_t, uint32_t, uint32_t, uint32_t);
int libkmem_copy_to_user(struct pcb_t*, uint32_t, uint32_t, uint32_t, uint32_t);
int libmem_dma_memcpy(struct pcb_t*, uint32_t, uint32_t, uint32_t, uint32_t);

/* helper kiem tra vung active (P3 goi truoc READ/WRITE) */
int vm_validate_active_region(struct pcb_t *caller, int rgid);
int vm_page_has_active_region(struct mm_struct *mm, addr_t page_vaddr);

/* slab pool kernel heap */
int    kmem_pool_grow(struct pcb_t* caller, struct kcache_pool_struct *pool);
addr_t kmem_pool_alloc_slot(struct pcb_t* caller, struct kcache_pool_struct *pool);
int    kmem_pool_free_slot( struct kcache_pool_struct *pool, addr_t kva);
int    kmem_pool_index(addr_t size);
int    kva_alloc(struct krnl_t* krnl, addr_t len, addr_t* start);
int    kva_free(struct krnl_t* krnl, addr_t start, addr_t len);
struct kmem_region* kregion_lookup(struct krnl_t* krnl, uint32_t pid, int reg);
int    __kfree(struct pcb_t* caller, int reg);
int    kmem_region_phys(struct krnl_t* krnl, uint32_t pid, int reg, addr_t off, addr_t* phys);
