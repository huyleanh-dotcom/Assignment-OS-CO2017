#ifndef COMMON_H
#define COMMON_H

/* Define structs and routine could be used by every source files */

#include <stdint.h>
#include <stdio.h>

#ifndef OSCFG_H
#include "os-cfg.h"
#endif

#ifndef OSMM_H
#include "os-mm.h"
#endif

/* so process toi da cho registry PID->PCB */
#define MAX_PROC 1024

#define ADDRESS_SIZE 20
#define OFFSET_LEN 10
#define FIRST_LV_LEN 5
#define SECOND_LV_LEN 5
#define SEGMENT_LEN FIRST_LV_LEN
#define PAGE_LEN SECOND_LV_LEN

#define NUM_PAGES (1 << (ADDRESS_SIZE - OFFSET_LEN))
#define PAGE_SIZE (1 << OFFSET_LEN)

/* 
 * @bksysnet: long address mode of 64bit argument support
 */

#ifdef MM64
#define ARG_TYPE uint64_t
#else
#define ARG_TYPE uint32_t
#endif

typedef ARG_TYPE arg_t;

/* 
 * @bksysnet: the format string need to be redefined
 *            based on the address mode
 */
#ifdef MM64
#define FORMAT_ARG "%lu"
#else
#define FORMAT_ARG "%u"
#endif


enum ins_opcode_t
{
	CALC,  // Just perform calculation, only use CPU
	ALLOC, // Allocate memory
	FREE,  // Deallocated a memory block
	READ,  // Write data to a byte on memory
	WRITE, // Read data from a byte on memory
	KMALLOC,           // Allocate kernel memory
	COPY_FROM_USER,    // Copy data from user memregion to kernel memregion
	COPY_TO_USER,      // Copy data from kernel memregion to user memregion
	DMA_MEMCPY, // Create kernel cache pool
	SYSCALL,
};

/* instructions executed by the CPU */
struct inst_t
{
	enum ins_opcode_t opcode;
	arg_t arg_0; // Argument lists for instructions
	arg_t arg_1;
	arg_t arg_2;
	arg_t arg_3;
};

struct code_seg_t
{
	struct inst_t *text;
	uint32_t size;
};

struct trans_table_t
{
	/* A row in the page table of the second layer */
	struct
	{
		addr_t v_index; // The index of virtual address
		addr_t p_index; // The index of physical address
	} table[1 << SECOND_LV_LEN];
	int size;
};

/* Mapping virtual addresses and physical ones */
struct page_table_t
{
	/* Translation table for the first layer */
	struct
	{
		addr_t v_index; // Virtual index
		struct trans_table_t *next_lv;
	} table[1 << FIRST_LV_LEN];
	int size; // Number of row in the first layer
};

/* PCB, describe information about a process */
struct pcb_t
{
	uint32_t pid;		 // PID
	uint32_t priority;	 // Default priority, this legacy process based (FIXED)
	char path[100];
	struct code_seg_t *code; // Code segment
	addr_t regs[10];	 // Registers, store address of allocated regions
	uint32_t pc;		 // Program pointer, point to the next instruction
#ifdef MLQ_SCHED
	// Priority on execution (if supported), on-fly aka. changeable
	// and this vale overwrites the default priority when it existed
	uint32_t prio;
#endif
	struct mm_struct *mm;
	struct krnl_t *krnl;	
	struct page_table_t *page_table; // Page table
	uint32_t bp;			 // Break pointer
};

/* Kernel structure */
struct krnl_t
{
	struct queue_t *ready_queue;
	struct queue_t *running_list;
#ifdef MLQ_SCHED
	struct queue_t *mlq_ready_queue;
#endif
#ifdef MM_PAGING
	struct mm_struct *mm;
	// struct vm_rg_struct symrgtbl[PAGING_MAX_SYMTBL_SZ];
	struct memphy_struct *mram;
	struct memphy_struct **mswp;
	struct memphy_struct *active_mswp;
	uint32_t active_mswp_id;
	struct kcache_pool_struct *kcpooltbl;

	/* registry PID->PCB: syscall nhan PID, tra PCB that o day
	 * (khong tin con tro PCB tu user). Lock bao ve: os_mm_lock (os.c). */
	struct pcb_t *pid_table[MAX_PROC];

#ifdef MM64
	addr_t *krnl_pgd;
	addr_t *krnl_p4d;
	addr_t *krnl_pud;
	addr_t *krnl_pmd;
	addr_t *krnl_pt;
#else
	uint32_t *krnl_pgd;
#endif
#endif
};

/* API registry (dinh nghia trong os.c) - tat ca tu lock os_mm_lock ben trong */
int  registry_register(struct krnl_t *krnl, struct pcb_t *proc);
struct pcb_t *registry_lookup(struct krnl_t *krnl, uint32_t pid);
int  registry_unregister(struct krnl_t *krnl, uint32_t pid);


#endif
