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
static volatile int done = 0;
static struct krnl_t os;

/* Global MM lock + registry PID->PCB.
 * De o day (khong o krnl_t) vi dua pthread_mutex_t vao common.h gay vong
 * include voi sched.h. Tach biet voi queue_lock cua scheduler. */
pthread_mutex_t os_mm_lock = PTHREAD_MUTEX_INITIALIZER;

/* kernel base macOS-style (khong phai Linux 0xFFFF8000...) */
#define KERNEL_BASE_ADDR 0xFFFFFF8000000000UL
unsigned long kernel_base = KERNEL_BASE_ADDR;

/* expose global MM lock de P3 khoa/mo ma khong dong truc tiep vao mutex
 * (tranh tu khoa lai non-recursive mutex). */
int mm_global_lock(void)   { return pthread_mutex_lock(&os_mm_lock); }
int mm_global_unlock(void) { return pthread_mutex_unlock(&os_mm_lock); }

/* Bang bam dia chi mo (open addressing), linear probe.
 * Dung tombstone cho o da xoa: lookup KHONG duoc dung tai o vua xoa (NULL that)
 * giua chuoi probe, neu khong se bo sot entry bi dich do va cham. */
#define REG_TOMBSTONE ((struct pcb_t *)-1)

int registry_register(struct krnl_t *krnl, struct pcb_t *proc)
{
	if (krnl == NULL || proc == NULL) return -1;
	pthread_mutex_lock(&os_mm_lock);
	int i, slot = proc->pid % MAX_PROC, first_free = -1;
	for (i = 0; i < MAX_PROC; i++) {
		int idx = (slot + i) % MAX_PROC;
		struct pcb_t *e = krnl->pid_table[idx];
		if (e == NULL) { if (first_free < 0) first_free = idx; break; }
		if (e == REG_TOMBSTONE) { if (first_free < 0) first_free = idx; continue; }
		if (e->pid == proc->pid) { pthread_mutex_unlock(&os_mm_lock); return -1; }
	}
	if (first_free < 0) { pthread_mutex_unlock(&os_mm_lock); return -1; }  /* bang day */
	krnl->pid_table[first_free] = proc;
	pthread_mutex_unlock(&os_mm_lock);
	return 0;
}

struct pcb_t *registry_lookup(struct krnl_t *krnl, uint32_t pid)
{
	if (krnl == NULL) return NULL;
	pthread_mutex_lock(&os_mm_lock);
	int i, slot = pid % MAX_PROC;
	struct pcb_t *found = NULL;
	for (i = 0; i < MAX_PROC; i++) {
		int idx = (slot + i) % MAX_PROC;
		struct pcb_t *e = krnl->pid_table[idx];
		if (e == NULL) break;                 /* het chuoi probe -> khong co */
		if (e == REG_TOMBSTONE) continue;     /* o da xoa -> di tiep, dung dung */
		if (e->pid == pid) { found = e; break; }
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
		struct pcb_t *e = krnl->pid_table[idx];
		if (e == NULL) break;
		if (e == REG_TOMBSTONE) continue;
		if (e->pid == pid) { krnl->pid_table[idx] = REG_TOMBSTONE; pthread_mutex_unlock(&os_mm_lock); return 0; }
	}
	pthread_mutex_unlock(&os_mm_lock);
	return -1;
}

/* duyet moi PCB con song, goi callback NGAY TRONG LUC giu lock (an toan cho
 * reclaim: PCB khong the bi go giua chung). callback tra khac 0 de dung som.
 * LUU Y: callback dang chay duoi os_mm_lock -> KHONG duoc goi lai cac ham
 * registry_* / mm_global_lock (se tu khoa). */
int registry_for_each_locked(struct krnl_t *krnl,
                             int (*callback)(struct pcb_t *, void *), void *arg)
{
	if (krnl == NULL || callback == NULL) return -1;
	pthread_mutex_lock(&os_mm_lock);
	int i, rc = 0;
	for (i = 0; i < MAX_PROC; i++) {
		struct pcb_t *e = krnl->pid_table[i];
		if (e == NULL || e == REG_TOMBSTONE) continue;
		rc = callback(e, arg);
		if (rc != 0) break;
	}
	pthread_mutex_unlock(&os_mm_lock);
	return rc;
}

/* duyet lan luot cac PCB con song trong registry (cho global reclaim cua P3).
 * *cursor khoi tao = 0; moi lan tra 1 PCB va day cursor; het thi tra NULL.
 *   int cur = 0; struct pcb_t *p;
 *   while ((p = registry_iter(krnl, &cur)) != NULL) { ... } */
struct pcb_t *registry_iter(struct krnl_t *krnl, int *cursor)
{
	if (krnl == NULL || cursor == NULL) return NULL;
	pthread_mutex_lock(&os_mm_lock);
	struct pcb_t *found = NULL;
	while (*cursor < MAX_PROC) {
		int idx = (*cursor)++;
		struct pcb_t *e = krnl->pid_table[idx];
		if (e != NULL && e != REG_TOMBSTONE) { found = e; break; }
	}
	pthread_mutex_unlock(&os_mm_lock);
	return found;
}

/*
 * registry_foreach_process - duyệt mọi process đang đăng ký
 *   Chép con trỏ PCB ra mảng tạm dưới os_mm_lock, rồi gọi cb NGOÀI lock:
 *   cb được phép lấy lock khác mà không giữ os_mm_lock lâu.
 *   cb trả != 0 -> dừng. Trả số process đã gọi cb, -1 nếu tham số sai.
 *
 *   Con trỏ chỉ còn hợp lệ khi bên gọi giữ mmvm_lock: process thoát làm
 *   registry_unregister TRƯỚC rồi mới free_pcb_memph (cần mmvm_lock) và
 *   free(mm), nên không thể bị giải phóng giữa chừng.
 *   Thứ tự lock toàn hệ thống: mmvm_lock -> os_mm_lock (không bao giờ ngược).
 */
int registry_foreach_process(struct krnl_t* krnl, int (*cb)(struct pcb_t*, void*), void* ctx)
{
	struct pcb_t* snap[MAX_PROC];
	int i, n = 0;

	if (krnl == NULL || cb == NULL) return -1;
	pthread_mutex_lock(&os_mm_lock);
	for (i = 0; i < MAX_PROC; i++)
		if (krnl->pid_table[i] != NULL && krnl->pid_table[i] != REG_TOMBSTONE)
			snap[n++] = krnl->pid_table[i];
	pthread_mutex_unlock(&os_mm_lock);

	for (i = 0; i < n; i++)
		if (cb(snap[i], ctx) != 0) return i + 1;
	return n;
}

/* don dep tap trung khi process exit: go registry roi giai phong user mm */
void kernel_cleanup_process(struct pcb_t *proc)
{
	if (proc == NULL) return;
	registry_unregister(&os, proc->pid);
#ifdef MM_PAGING
/* trả frame RAM + SWAP slot, rồi pt_destroy_tree; cần proc->mm còn sống */
	kmem_release_process(proc);
	free_pcb_memph(proc);
#endif
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
	struct memphy_struct *mswp[PAGING_MAX_MMSWP];
	struct memphy_struct *active_mswp;
	int active_mswp_id;
	struct timer_id_t  *timer_id;
};

/* Keep the pointer table alive for as long as the loader arguments. */
static int
init_memory_devices (struct mmpaging_ld_args *args,
                     struct memphy_struct *ram,
                     struct memphy_struct swaps[PAGING_MAX_MMSWP])
{
  int id;

  args->mram = ram;
  args->active_mswp = NULL;
  args->active_mswp_id = -1;
  for (id = 0; id < PAGING_MAX_MMSWP; id++)
    args->mswp[id] = NULL;

  if (init_memphy (ram, memramsz, 1) != 0)
    return -1;

  for (id = 0; id < PAGING_MAX_MMSWP; id++)
    {
      if (init_memphy (&swaps[id], memswpsz[id], 1) != 0)
        return -1;
#ifdef MM64
      if (swaps[id].maxsz < PAGING64_PAGESZ)
#else
      if (swaps[id].maxsz < PAGING_PAGESZ)
#endif
        continue;

      args->mswp[id] = &swaps[id];
      if (args->active_mswp == NULL)
        {
          args->active_mswp = &swaps[id];
          args->active_mswp_id = id;
        }
    }

  return ram->maxsz > 0 && args->active_mswp != NULL ? 0 : -1;
}
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
			//if (proc == NULL) {
   //                        next_slot(timer_id);
   //                        continue; /* First load failed. skip dummy load */
   //                     }
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
	if (dma_engine_init() != 0 || dma_notifier_init() != 0) 
	{
		printf("Cannot start DMA engine/notifier threads\n");
		return (void*)1;
	}
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
	os.active_mswp_id = ((struct mmpaging_ld_args *)args)->active_mswp_id;
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
#ifdef MM_PAGING
/* Print memory access counts and page-table storage at the end of a run */
static void print_memory_stats(struct memphy_struct *ram,
                               struct memphy_struct swaps[PAGING_MAX_MMSWP])
{
	struct pt_table_stats st;
	int id;

	pt_page_table_stats(NULL, &st);
	printf("===== Memory statistics =====\n");
	printf("RAM   : %d B, %lu byte reads, %lu byte writes\n",
	       ram->maxsz, ram->nread, ram->nwrite);
	for (id = 0; id < PAGING_MAX_MMSWP; id++) 
	{
		if (swaps[id].maxsz > 0) 
		{
			printf("SWAP%d : %d B, %lu byte reads, %lu byte writes\n",
			       id, swaps[id].maxsz, swaps[id].nread, swaps[id].nwrite);
		}
	}
	printf("Page-table walks: %lu (up to 5 table levels per walk)\n", st.walks);
	printf("Page-table storage: peak %lu tables = %lu B, at exit %lu tables\n",
	       st.peak_tables, st.peak_tables * 4096UL, st.total_tables);
}
#endif

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
	/* Default memory sizes, used when the config file has no memory line
	 * (legacy format, e.g. input/sched): 2 MiB RAM and one 16 MiB SWAP.
	 * Keep RAM within the int-sized memphy_struct.maxsz. */
	memramsz = PAGING_MEMRAMSZ;
	memswpsz[0] = 0x1000000;
	for (sit = 1; sit < PAGING_MAX_MMSWP; sit++)
		memswpsz[sit] = 0;
#ifndef MM_FIXED_MEMSZ
#ifdef MM64_KERNEL_BASE_IN_CONFIG
	/* format mo rong (spec 2.2.5): dong KERNEL_BASE truoc mem sizes.
	 * Input mau khong co dong nay nen mac dinh dung KERNEL_BASE_ADDR (macOS). */
	fscanf(file, "%lu\n", &kernel_base);
#endif
	/* Read input config of memory size: MEMRAM and upto 4 MEMSWP (mem swap)
	 * Format: (size=0 result non-used memswap, must have RAM and at least 1 SWAP)
	 *        MEM_RAM_SZ MEM_SWP0_SZ MEM_SWP1_SZ MEM_SWP2_SZ MEM_SWP3_SZ
	*/
	/* The memory line has 5 numbers, while a process line has the form
	 * [time] [name] [prio]. If the second line is not a memory line,
	 * keep the default sizes and leave the line for the process loop. */
	long line_start = ftell(file);
	char line[256];
	arg_t mem[1 + PAGING_MAX_MMSWP];
	if (fgets(line, sizeof(line), file) != NULL &&
	    sscanf(line, FORMAT_ARG " " FORMAT_ARG " " FORMAT_ARG " " FORMAT_ARG " " FORMAT_ARG,
	           &mem[0], &mem[1], &mem[2], &mem[3], &mem[4]) == 1 + PAGING_MAX_MMSWP) {
		memramsz = mem[0];
		for (sit = 0; sit < PAGING_MAX_MMSWP; sit++)
			memswpsz[sit] = mem[sit + 1];
	} else {
		fseek(file, line_start, SEEK_SET);
	}
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

	os.kcpooltbl = (struct kcache_pool_struct*)
		calloc(KMEM_NUM_POOLS, sizeof(struct kcache_pool_struct));
	{
		int kp_sizes[KMEM_NUM_POOLS] = { 16, 32, 64, 96, 128, 192, 256, 512, 1024, 2048 };
		int kp;
		for (kp = 0; kp < KMEM_NUM_POOLS; kp++) {
			os.kcpooltbl[kp].size = kp_sizes[kp];
			os.kcpooltbl[kp].align = 16;
			os.kcpooltbl[kp].slots_per_chunk = KMEM_CHUNK_SIZE / kp_sizes[kp];
			os.kcpooltbl[kp].chunks = NULL;
			os.kcpooltbl[kp].nchunks = 0;
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
	struct memphy_struct mram = {0};
	struct memphy_struct mswp[PAGING_MAX_MMSWP] = {0};

	/* In Paging mode, it needs passing the system mem to each PCB through loader*/
	struct mmpaging_ld_args *mm_ld_args = calloc(1, sizeof(*mm_ld_args));
	if (mm_ld_args == NULL || init_memory_devices(mm_ld_args, &mram, mswp) != 0)
	{
		fprintf(stderr, "Cannot initialize RAM/SWAP: require positive RAM and a usable SWAP device\n");
		exit(EXIT_FAILURE);
	}

	mm_ld_args->timer_id = ld_event;


#endif

	/* Init scheduler */
	init_scheduler();
	
	/* Start the co-processor first, so DMA is ready before any process runs */
	pthread_create(dma, NULL, co_processor, NULL);
	pthread_join(*dma, NULL);

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

	/* Wait for CPU and loader finishing */
	for (i = 0; i < num_cpus; i++) {
		pthread_join(cpu[i], NULL);
	}
	pthread_join(ld, NULL);

	/* Stop timer */
	stop_timer();

	/* Finish pending DMA jobs and interrupts, then free the RTL model */
	dma_worker_shutdown();
	dma_notifier_shutdown();
	rtl_dma_cleanup_simulation();
	free(dma);

#ifdef MM_PAGING
	print_memory_stats(&mram, mswp);
#endif
	return 0;

}



