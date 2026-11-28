/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* LamiaAtrium release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

#include "queue.h"
#include "sched.h"
#include <pthread.h>

#include <stdlib.h>
#include <stdio.h>
static struct queue_t ready_queue;
static struct queue_t run_queue;
static pthread_mutex_t queue_lock;

static struct queue_t running_list;
/* Processes blocked until their DMA completion interrupt arrives */
static struct queue_t dma_waiting_list;
/* DMA jobs not finished yet, one entry per process that has some
* (static storage starts as all zero). */
static struct 
{
	struct pcb_t *proc;
	int pending;
} dma_pending_tbl[MAX_QUEUE_SIZE];

#ifdef MLQ_SCHED
static struct queue_t mlq_ready_queue[MAX_PRIO];
static int slot[MAX_PRIO];
#endif

int queue_empty(void) {
#ifdef MLQ_SCHED
	unsigned long prio;
	for (prio = 0; prio < MAX_PRIO; prio++)
		if(!empty(&mlq_ready_queue[prio])) 
			return -1;
#endif
	return (empty(&ready_queue) && empty(&run_queue));
}

void init_scheduler(void) {
#ifdef MLQ_SCHED
    int i ;

	for (i = 0; i < MAX_PRIO; i ++) {
		mlq_ready_queue[i].size = 0;
		slot[i] = MAX_PRIO - i; 
	}
#endif
	ready_queue.size = 0;
	run_queue.size = 0;
	running_list.size = 0;
	dma_waiting_list.size = 0;
	pthread_mutex_init(&queue_lock, NULL);
}

#ifdef MLQ_SCHED
/* 
 *  Stateful design for routine calling
 *  based on the priority and our MLQ policy
 *  We implement stateful here using transition technique
 *  State representation   prio = 0 .. MAX_PRIO, curr_slot = 0..(MAX_PRIO - prio)
 */




struct pcb_t * get_mlq_proc(void) {
	struct pcb_t * proc = NULL;
	int i, pass, has_proc;
	pthread_mutex_lock(&queue_lock);
	/*TODO: get a process from PRIORITY [ready_queue].
	 *      It worth to protect by a mechanism.
	 * */
	/* At most two passes: the second pass runs only after a slot reset */
	for (pass = 0; pass < 2 && proc == NULL; pass++) 
	{
		has_proc = 0;
		/* Always scan from the highest priority (prio = 0) */
		for (i = 0; i < MAX_PRIO; i++) 
		{
			if (empty(&mlq_ready_queue[i]))
			{
				continue;
			}
			has_proc = 1;
			if (slot[i] > 0) 
			{
				proc = dequeue(&mlq_ready_queue[i]);
				slot[i]--;          /* one dispatch consumes one slot */
				break;
			}
		}
		if (!has_proc)
		{
			break;                  /* all queues are empty */
		}
		if (proc == NULL) 
		{
			/* Every non-empty queue is out of slot: reset all 140 queues */
			for (i = 0; i < MAX_PRIO; i++)
			{
				slot[i] = MAX_PRIO - i;
			}
		}
	}
	if (proc != NULL)
	{
		enqueue(&running_list, proc);
	}
	pthread_mutex_unlock(&queue_lock);
	return proc;	
}

/* prio is unsigned; a value past the last level goes to the lowest level. */
static void clamp_prio(struct pcb_t *proc)
{
	if (proc->prio >= MAX_PRIO)
	{
		proc->prio = MAX_PRIO - 1;
	}
}

void put_mlq_proc(struct pcb_t * proc) {
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->mlq_ready_queue = mlq_ready_queue;
	proc->krnl->running_list = &running_list;
	clamp_prio(proc);
	/* TODO: put running proc to running_list 
	 *       It worth to protect by a mechanism.
	 * 
	 */
	
	pthread_mutex_lock(&queue_lock);
	purgequeue(&running_list, proc);
	enqueue(&mlq_ready_queue[proc->prio], proc);
	pthread_mutex_unlock(&queue_lock);
}

void add_mlq_proc(struct pcb_t * proc) {
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->mlq_ready_queue = mlq_ready_queue;
	proc->krnl->running_list = &running_list;
	clamp_prio(proc);

	/* TODO: put running proc to running_list
	 *       It worth to protect by a mechanism.
	 * 
	 */
       
	pthread_mutex_lock(&queue_lock);
	enqueue(&mlq_ready_queue[proc->prio], proc);
	pthread_mutex_unlock(&queue_lock);	
}

struct pcb_t * get_proc(void) {
	return get_mlq_proc();
}

void put_proc(struct pcb_t * proc) {
	return put_mlq_proc(proc);
}

void add_proc(struct pcb_t * proc) {
	return add_mlq_proc(proc);
}
#else
struct pcb_t * get_proc(void) {
	struct pcb_t * proc = NULL;

	pthread_mutex_lock(&queue_lock);
	/*TODO: get a process from [ready_queue].
	 *       It worth to protect by a mechanism.
	 * 
	 */

	pthread_mutex_unlock(&queue_lock);

	return proc;
}

void put_proc(struct pcb_t * proc) {
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;

	/* TODO: put running proc to running_list 
	 *       It worth to protect by a mechanism.
	 * 
	 */

	pthread_mutex_lock(&queue_lock);
	enqueue(&run_queue, proc);
	pthread_mutex_unlock(&queue_lock);
}

void add_proc(struct pcb_t * proc) {
	proc->krnl->ready_queue = &ready_queue;
	proc->krnl->running_list = &running_list;

	/* TODO: put running proc to running_list 
	 *       It worth to protect by a mechanism.
	 * 
	 */

	pthread_mutex_lock(&queue_lock);
	enqueue(&ready_queue, proc);
	pthread_mutex_unlock(&queue_lock);	
}
#endif
void finish_proc(struct pcb_t *proc)
{
	pthread_mutex_lock(&queue_lock);
	purgequeue(&running_list, proc);
	pthread_mutex_unlock(&queue_lock);
}

/* Entry of @proc in dma_pending_tbl, or NULL. Caller holds queue_lock. */
static int *dma_pending_of(struct pcb_t *proc)
{
	int i;

	for (i = 0; i < MAX_QUEUE_SIZE; i++) 
	{
		if (dma_pending_tbl[i].proc == proc) 
		{
			return &dma_pending_tbl[i].pending;
		}
	}
	return NULL;
}

/* Decrease the count of @proc; free its entry when it reaches 0.
 * Return the new count. Caller holds queue_lock. */
static int dma_pending_dec(struct pcb_t *proc)
{
	int i;

	for (i = 0; i < MAX_QUEUE_SIZE; i++) 
	{
		if (dma_pending_tbl[i].proc == proc) 
		{
			dma_pending_tbl[i].pending--;
			if (dma_pending_tbl[i].pending <= 0) 
			{
				dma_pending_tbl[i].proc = NULL;
				dma_pending_tbl[i].pending = 0;
				return 0;
			}
			return dma_pending_tbl[i].pending;
		}
	}
	return 0;
}

/* Called before a DMA job is submitted: one more job to wait for */
void dma_wait_begin(struct pcb_t *proc)
{
	int *pending;
	int i;

	pthread_mutex_lock(&queue_lock);
	pending = dma_pending_of(proc);
	if (pending != NULL) {
		(*pending)++;
	} 
	else 
	{
		for (i = 0; i < MAX_QUEUE_SIZE; i++) 
		{
			if (dma_pending_tbl[i].proc == NULL) 
			{
				dma_pending_tbl[i].proc = proc;
				dma_pending_tbl[i].pending = 1;
				break;
			}
		}
	}
	pthread_mutex_unlock(&queue_lock);
}

/* Called when the DMA job could not be submitted */
void dma_wait_abort(struct pcb_t *proc)
{
	pthread_mutex_lock(&queue_lock);
	dma_pending_dec(proc);
	pthread_mutex_unlock(&queue_lock);
}
