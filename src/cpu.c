/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * CPU and co-processor module
 */
#include "cpu.h"
#include "mem.h"
#include "mm.h"
#include "syscall.h"
#include "libmem.h"
#ifdef MM_PAGING
/* Return the name of a memory instruction and its number of arguments.
 * Return NULL if the opcode is not a memory instruction. */
static const char *mem_op_name(enum ins_opcode_t opcode, int *nargs)
{
	switch (opcode) 
	{
	case ALLOC:
		*nargs = 2;
		return "alloc";
	case FREE:
		*nargs = 1;
		return "free";
	case READ:
		*nargs = 3;
		return "read";
	case WRITE:
		*nargs = 3;
		return "write";
	case KMALLOC:
		*nargs = 2;
		return "kmalloc";
	case COPY_FROM_USER:
		*nargs = 4;
		return "copy_from_user";
	case COPY_TO_USER:
		*nargs = 4;
		return "copy_to_user";
	case DMA_MEMCPY:
		*nargs = 4;
		return "dma_memcpy";
	default:
		return NULL;
	}
}

/* Print one line when a memory instruction is rejected */
static void report_invalid_mem_op(struct pcb_t *proc, struct inst_t ins)
{
	unsigned long long args[4] = {
		(unsigned long long)ins.arg_0, (unsigned long long)ins.arg_1,
		(unsigned long long)ins.arg_2, (unsigned long long)ins.arg_3
	};
	int nargs = 0;
	int i;
	const char *name = mem_op_name(ins.opcode, &nargs);

	if (name == NULL) 
	{
		return;
	}
	printf("\tPID %d: invalid memory operation: %s", proc->pid, name);
	for (i = 0; i < nargs; i++) 
	{
		printf(" %llu", args[i]);
	}
	printf("\n");
}
#endif


int calc(struct pcb_t *proc)
{
	return ((unsigned long)proc & 0UL);
}

int alloc(struct pcb_t *proc, uint32_t size, uint32_t reg_index)
{
	addr_t addr = alloc_mem(size, proc);
	if (addr == 0)
	{
		return 1;
	}
	else
	{
		proc->regs[reg_index] = addr;
		return 0;
	}
}

int free_data(struct pcb_t *proc, uint32_t reg_index)
{
	return free_mem(proc->regs[reg_index], proc);
}

int read(
	struct pcb_t *proc, // Process executing the instruction
	uint32_t source,	// Index of source register
	uint32_t offset,	// Source address = [source] + [offset]
	uint32_t destination)
{ // Index of destination register

	BYTE data;
	if (read_mem(proc->regs[source] + offset, proc, &data))
	{
		proc->regs[destination] = data;
		return 0;
	}
	else
	{
		return 1;
	}
}

int cpu_write(
	struct pcb_t *proc,	// Process executing the instruction
	BYTE data,		// Data to be written into memory
	uint32_t destination, // Index of destination register
	uint32_t offset)
{ // Destination address =
	// [destination] + [offset]
	return write_mem(proc->regs[destination] + offset, proc, data);
}

int run(struct pcb_t *proc)
{
	/* Check if Program Counter point to the proper instruction */
	if (proc->pc >= proc->code->size)
	{
		return 1;
	}

	struct inst_t ins = proc->code->text[proc->pc];
	proc->pc++;
	int stat = 1;
switch (ins.opcode)
	{
	case CALC:
		stat = calc(proc);
		break;
	case ALLOC:
#ifdef MM_PAGING
		stat = liballoc(proc, ins.arg_0, ins.arg_1);
#else
		stat = alloc(proc, ins.arg_0, ins.arg_1);
#endif
		break;
#ifdef MM_PAGING		
	case KMALLOC:
		stat = libkmem_malloc(proc, ins.arg_0, ins.arg_1);
		break;
	case COPY_FROM_USER:
		stat = libkmem_copy_from_user(proc, ins.arg_0, ins.arg_1, ins.arg_2, ins.arg_3);
		break;
	case COPY_TO_USER:
		stat = libkmem_copy_to_user(proc, ins.arg_0, ins.arg_1, ins.arg_2, ins.arg_3);
		break;
	case DMA_MEMCPY:
		stat = libmem_dma_memcpy(proc, ins.arg_0, ins.arg_1, ins.arg_2, ins.arg_3);
		break;
#endif
	case FREE:
#ifdef MM_PAGING
		stat = libfree(proc, ins.arg_0);
#else
		stat = free_data(proc, ins.arg_0);
#endif
		break;
	case READ:
#ifdef MM_PAGING
	{
		/* read [source][offset][destination]: luu byte doc duoc
		 * vao thanh ghi dich regs[arg_2], khong ghi vao operand */
		uint32_t rd_val = 0;
		stat = libread(proc, ins.arg_0, ins.arg_1, &rd_val);
		proc->regs[ins.arg_2] = rd_val;
	}
#else
		stat = read(proc, ins.arg_0, ins.arg_1, ins.arg_2);
#endif
		break;
	case WRITE:
#ifdef MM_PAGING
		stat = libwrite(proc, ins.arg_0, ins.arg_1, ins.arg_2);
#else
		stat = cpu_write(proc, ins.arg_0, ins.arg_1, ins.arg_2);
#endif
		break;
	case SYSCALL:
		stat = libsyscall(proc, ins.arg_0, ins.arg_1, ins.arg_2, ins.arg_3);
		break;
	default:
		stat = 1;
	}
#ifdef MM_PAGING
	if (stat != 0) 
	{
		report_invalid_mem_op(proc, ins);
	}
#endif
	return stat;
}
