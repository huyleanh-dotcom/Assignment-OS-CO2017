/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * System OS module
 */
#include "cpu.h"
#include "timer.h"
#include "sched.h"
#include "loader.h"
#include "mm.h"
#ifdef MM64
#include "mm64.h"
#endif

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include "sysbus.h"
#include "dpi_rtl.h"

static int time_slot;
static int num_cpus;
static int done = 0;
static struct krnl_t os;

/* Global MM lock + registry PID->PCB.
 * De o day (khong o krnl_t) vi dua pthread_mutex_t vao common.h gay vong
 * include voi sched.h. Tach biet voi queue_lock cua scheduler. */
pthread_mutex_t os_mm_lock = PTHREAD_MUTEX_INITIALIZER;

/* kernel base macOS-style (khong phai Linux 0xFFFF8000...) */
#define KERNEL_BASE_ADDR 0xFFFFFF8000000000UL
unsigned long kernel_base = KERNEL_BASE_ADDR;

int registry_register(struct krnl_t *krnl, struct pcb_t *proc)
{
	if (krnl == NULL || proc == NULL) return -1;
	pthread_mutex_lock(&os_mm_lock);
	int i, slot = proc->pid % MAX_PROC;
	for (i = 0; i < MAX_PROC; i++) {
		int idx = (slot + i) % MAX_PROC;
		if (krnl->pid_table[idx] == NULL) { krnl->pid_table[idx] = proc; pthread_mutex_unlock(&os_mm_lock); return 0; }
		if (krnl->pid_table[idx]->pid == proc->pid) { pthread_mutex_unlock(&os_mm_lock); return -1; }
	}
	pthread_mutex_unlock(&os_mm_lock);
	return -1;
}

struct pcb_t *registry_lookup(struct krnl_t *krnl, uint32_t pid)
{
	if (krnl == NULL) return NULL;
	pthread_mutex_lock(&os_mm_lock);
	int i, slot = pid % MAX_PROC;
	struct pcb_t *found = NULL;
	for (i = 0; i < MAX_PROC; i++) {
		int idx = (slot + i) % MAX_PROC;
		if (krnl->pid_table[idx] == NULL) break;
		if (krnl->pid_table[idx]->pid == pid) { found = krnl->pid_table[idx]; break; }
	}
	pthread_mutex_unlock(&os_mm_lock);
	return found;
}

int registry_unregister(struct krnl_t *krnl, uint32_t pid)
{
	if (krnl == NULL) return -1;
	pthread_mutex_lock(&os_mm_lock);
	int i, slot = pid % MAX_PROC;
	for (i = 0; i < MAX_PROC; i++) {
		int idx = (slot + i) % MAX_PROC;
		if (krnl->pid_table[idx] == NULL) break;
		if (krnl->pid_table[idx]->pid == pid) { krnl->pid_table[idx] = NULL; pthread_mutex_unlock(&os_mm_lock); return 0; }
	}
	pthread_mutex_unlock(&os_mm_lock);
	return -1;
}

/* don dep tap trung khi process exit: go registry roi giai phong user mm */
void kernel_cleanup_process(struct pcb_t *proc)
{
	if (proc == NULL) return;
	registry_unregister(&os, proc->pid);
	/* P1 them pt_destroy_tree(proc->mm); P3 them free_pcb_memph(proc) tai day */
	if (proc->mm != NULL) {
		struct vm_area_struct *vma = proc->mm->mmap;
		while (vma != NULL) {
			struct vm_area_struct *nextvma = vma->vm_next;
			struct vm_rg_struct *rg = vma->vm_freerg_list;
			while (rg != NULL) { struct vm_rg_struct *n = rg->rg_next; free(rg); rg = n; }
			free(vma);
			vma = nextvma;
		}
		free(proc->mm);
		proc->mm = NULL;
	}
}

#ifdef MM_PAGING
static unsigned long memramsz;
static unsigned long memswpsz[PAGING_MAX_MMSWP];

struct mmpaging_ld_args {
	/* A dispatched argument struct to compact many-fields passing to loader */
	int vmemsz;
	struct memphy_struct *mram;
	struct memphy_struct **mswp;
	struct memphy_struct *active_mswp;
	int active_mswp_id;
	struct timer_id_t  *timer_id;
};
#endif

static struct ld_args{
	char ** path;
	unsigned long * start_time;
#ifdef MLQ_SCHED
	unsigned long * prio;
#endif
} ld_processes;
int num_processes;

struct cpu_args {
	struct timer_id_t * timer_id;
	int id;
};

static void * cpu_routine(void * args) {
	struct timer_id_t * timer_id = ((struct cpu_args*)args)->timer_id;
	int id = ((struct cpu_args*)args)->id;
	/* Check for new process in ready queue */
	int time_left = 0;
	struct pcb_t * proc = NULL;
	while (1) {
		/* Check the status of current process */
		if (proc == NULL) {
			/* No process is running, the we load new process from
		 	* ready queue */
			proc = get_proc();
			if (proc == NULL) {
                           next_slot(timer_id);
                           continue; /* First load failed. skip dummy load */
                        }
		}else if (proc->pc == proc->code->size) {
			/* The porcess has finish it job */
			printf("\tCPU %d: Processed %2d has finished\n",
				id ,proc->pid);
			finish_proc(proc);

			kernel_cleanup_process(proc);   /* don dep mm + registry */

			free(proc);
			proc = get_proc();
			time_left = 0;
		}else if (time_left == 0) {
			/* The process has done its job in current time slot */
			printf("\tCPU %d: Put process %2d to run queue\n",
				id, proc->pid);
			put_proc(proc);
			proc = get_proc();
		}
		
		/* Recheck process status after loading new process */
		if (proc == NULL && done) {
			/* No process to run, exit */
			printf("\tCPU %d stopped\n", id);
			break;
		}else if (proc == NULL) {
			/* There may be new processes to run in
			 * next time slots, just skip current slot */
			next_slot(timer_id);
			continue;
		}else if (time_left == 0) {
			printf("\tCPU %d: Dispatched process %2d\n",
				id, proc->pid);
			time_left = time_slot;
		}
		
		/* Run current process */
		run(proc);
		time_left--;
		next_slot(timer_id);
	}
	detach_event(timer_id);
	pthread_exit(NULL);
}


static void * co_processor(void *arg) {
    (void)arg;
    /* TODO: Initialize the internal Verilator RTL simulation.
	 *       This is necessary to ensure that the RTL simulation is ready for DPI calls.
      */
    if (rtl_dma_init_simulation() != 0) {
        printf("Cannot initialize RTL simulation\n");
        return (void*)1;
    }
	
	/* TODO: This asynchronous implementation of DMA
     *        DMA completion callback is not yet implemented in this testbench
     */
    // Install callback handler
    //dma_done_callback();  // Call the callback to signal DMA completion
    return NULL;
}

static void * ld_routine(void * args) {
#ifdef MM_PAGING
	struct memphy_struct* mram = ((struct mmpaging_ld_args *)args)->mram;
	struct memphy_struct** mswp = ((struct mmpaging_ld_args *)args)->mswp;
	struct memphy_struct* active_mswp = ((struct mmpaging_ld_args *)args)->active_mswp;
	struct timer_id_t * timer_id = ((struct mmpaging_ld_args *)args)->timer_id;
#else
	struct timer_id_t * timer_id = (struct timer_id_t*)args;
#endif
	int i = 0;
  /* TODO init kernel page table directory */
#ifdef MM64
	os.krnl_pgd = NULL;
	os.krnl_p4d = NULL;
	os.krnl_pud = NULL;
	os.krnl_pmd = NULL;
	os.krnl_pt = NULL;
#else
	os.krnl_pgd = (uint32_t*)malloc(PAGING_MAX_PGN * sizeof(uint32_t));
#endif
	i=0;
	printf("ld_routine\n");
	os.mram = mram;
	os.mswp = mswp;
	os.active_mswp = active_mswp;
	os.mm = (struct mm_struct*)malloc(sizeof(struct mm_struct));
	init_mm(os.mm, NULL);
#ifdef MM64
	os.krnl_pgd = os.mm->pgd;   /* Kernel PGD chung, alias */
#endif
	while (i < num_processes) {
		struct pcb_t * proc = load(ld_processes.path[i]);
		proc->krnl = &os;

#ifdef MLQ_SCHED
	proc->prio = ld_processes.prio[i];
#endif
	while (current_time() < ld_processes.start_time[i]) {
		next_slot(timer_id);
	}
#ifdef MM_PAGING
	proc->mm = (struct mm_struct*)malloc(sizeof(struct mm_struct));
	init_mm(proc->mm, proc);
#endif
		/* dang ky PID->PCB de syscall tra duoc PCB that */
		if (registry_register(&os, proc) != 0)
			printf("registry_register failed for PID %d\n", proc->pid);
		printf("\tLoaded a process at %s, PID: %d PRIO: %ld\n",
			ld_processes.path[i], proc->pid, ld_processes.prio[i]);
		add_proc(proc);
		free(ld_processes.path[i]);
		i++;
		next_slot(timer_id);
	}
	free(ld_processes.path);
	free(ld_processes.start_time);
	done = 1;
	detach_event(timer_id);
	pthread_exit(NULL);
}

static void read_config(const char * path) {
	FILE * file;
	if ((file = fopen(path, "r")) == NULL) {
		printf("Cannot find configure file at %s\n", path);
		exit(1);
	}
	fscanf(file, "%d %d %d\n", &time_slot, &num_cpus, &num_processes);
	ld_processes.path = (char**)malloc(sizeof(char*) * num_processes);
	ld_processes.start_time = (unsigned long*)
		malloc(sizeof(unsigned long) * num_processes);
#ifdef MM_PAGING
	int sit;
#ifdef MM_FIXED_MEMSZ
	/* We provide here a back compatible with legacy OS simulatiom config file
         * In which, it have no addition config line for Mema, keep only one line
	 * for legacy info 
         *  [time slice] [N = Number of CPU] [M = Number of Processes to be run]
         */
        memramsz  =  0x100000000;
        memswpsz[0] = 0x1000000;
	for(sit = 1; sit < PAGING_MAX_MMSWP; sit++)
		memswpsz[sit] = 0;
#else
#ifdef MM64_KERNEL_BASE_IN_CONFIG
	/* format mo rong (spec 2.2.5): dong KERNEL_BASE truoc mem sizes.
	 * Input mau khong co dong nay nen mac dinh dung KERNEL_BASE_ADDR (macOS). */
	fscanf(file, "%lu\n", &kernel_base);
#endif
	/* Read input config of memory size: MEMRAM and upto 4 MEMSWP (mem swap)
	 * Format: (size=0 result non-used memswap, must have RAM and at least 1 SWAP)
	 *        MEM_RAM_SZ MEM_SWP0_SZ MEM_SWP1_SZ MEM_SWP2_SZ MEM_SWP3_SZ
	*/
	fscanf(file, FORMAT_ARG "\n", &memramsz);
	for(sit = 0; sit < PAGING_MAX_MMSWP; sit++)
		fscanf(file, FORMAT_ARG, &(memswpsz[sit])); 

       fscanf(file, "\n"); /* Final character */
#endif
#endif

#ifdef MLQ_SCHED
	ld_processes.prio = (unsigned long*)
		malloc(sizeof(unsigned long) * num_processes);
#endif
	int i;
	for (i = 0; i < num_processes; i++) {
		ld_processes.path[i] = (char*)malloc(sizeof(char) * 100);
		ld_processes.path[i][0] = '\0';
		strcat(ld_processes.path[i], "input/proc/");
		char proc[100];
#ifdef MLQ_SCHED
		fscanf(file, "%lu %s %lu\n", &ld_processes.start_time[i], proc, &ld_processes.prio[i]);
#else
		fscanf(file, "%lu %s\n", &ld_processes.start_time[i], proc);
#endif
		strcat(ld_processes.path[i], proc);
	}
}

int main(int argc, char * argv[]) {
	/* Read config */
	if (argc != 2) {
		printf("Usage: os [path to configure file]\n");
		return 1;
	}
	char path[100];
	path[0] = '\0';
	strcat(path, "input/");
	strcat(path, argv[1]);
	read_config(path);

	/* khoi tao registry + kernel cache pools truoc khi cac thread chay */
	memset(os.pid_table, 0, sizeof(os.pid_table));
#define NUM_KCACHE_POOL 5
	os.kcpooltbl = (struct kcache_pool_struct*)
		calloc(NUM_KCACHE_POOL, sizeof(struct kcache_pool_struct));
	{
		int kp_sizes[NUM_KCACHE_POOL] = {32, 64, 128, 256, 512};
		int kp;
		for (kp = 0; kp < NUM_KCACHE_POOL; kp++) {
			os.kcpooltbl[kp].size = kp_sizes[kp];
			os.kcpooltbl[kp].align = 8;
			os.kcpooltbl[kp].slots_per_chunk = 8;
			os.kcpooltbl[kp].free_list = NULL;
		}
	}

	pthread_t * cpu = (pthread_t*)malloc(num_cpus * sizeof(pthread_t));
	pthread_t * dma = (pthread_t*)malloc(sizeof(pthread_t));
	struct cpu_args * args =
		(struct cpu_args*)malloc(sizeof(struct cpu_args) * num_cpus);
	pthread_t ld;
	
	/* Init timer */
	int i;
	for (i = 0; i < num_cpus; i++) {
		args[i].timer_id = attach_event();
		args[i].id = i;
	}
	struct timer_id_t * ld_event = attach_event();
	start_timer();

#ifdef MM_PAGING
	/* Init all MEMPHY include 1 MEMRAM and n of MEMSWP */
	int rdmflag = 1; /* By default memphy is RANDOM ACCESS MEMORY */

	struct memphy_struct mram;
	struct memphy_struct mswp[PAGING_MAX_MMSWP];

	/* Create MEM RAM */
	init_memphy(&mram, memramsz, rdmflag);

        /* Create all MEM SWAP */ 
	int sit;
	for(sit = 0; sit < PAGING_MAX_MMSWP; sit++)
	       init_memphy(&mswp[sit], memswpsz[sit], rdmflag);

	/* In Paging mode, it needs passing the system mem to each PCB through loader*/
	struct mmpaging_ld_args *mm_ld_args = (struct mmpaging_ld_args*)malloc(sizeof(struct mmpaging_ld_args));

	mm_ld_args->timer_id = ld_event;
	mm_ld_args->mram = (struct memphy_struct *) &mram;
	mm_ld_args->mswp = (struct memphy_struct**) &mswp;
	mm_ld_args->active_mswp = (struct memphy_struct *) &mswp[0];
        mm_ld_args->active_mswp_id = 0;


#endif

	/* Init scheduler */
	init_scheduler();

	/* Run CPU and loader */
#ifdef MM_PAGING
	pthread_create(&ld, NULL, ld_routine, (void*)mm_ld_args);
#else
	pthread_create(&ld, NULL, ld_routine, (void*)ld_event);
#endif
	for (i = 0; i < num_cpus; i++) {
		pthread_create(&cpu[i], NULL,
			cpu_routine, (void*)&args[i]);
	}

	/* Run co-processor */
	pthread_create(dma, NULL, co_processor, NULL);

	/* Wait for CPU and loader finishing */
	for (i = 0; i < num_cpus; i++) {
		pthread_join(cpu[i], NULL);
	}
	pthread_join(ld, NULL);

	/* Stop timer */
	stop_timer();

	return 0;

}



