#include <stdio.h>
#include <stdlib.h>
#include "queue.h"

int empty(struct queue_t *q)
{
        if (q == NULL)
                return 1;
        return (q->size == 0);
}

/* Remove the element at index IDX, keeping the order of the rest. */
static struct pcb_t *remove_at(struct queue_t *q, int idx)
{
        struct pcb_t *proc = q->proc[idx];
        int i;

        for (i = idx; i < q->size - 1; i++)
        {
                q->proc[i] = q->proc[i + 1];
        }
        q->size--;
        q->proc[q->size] = NULL;
        return proc;
}

void enqueue(struct queue_t *q, struct pcb_t *proc)
{
        /* TODO: put a new process to queue [q] */
        if (q == NULL || proc == NULL)
        {
                return;
        }
        if (q->size >= MAX_QUEUE_SIZE)
        {
                fprintf(stderr, "enqueue: queue full (%d), drop pid %d\n",
                        MAX_QUEUE_SIZE, proc->pid);
                return;
        }
        q->proc[q->size++] = proc;
}

struct pcb_t *dequeue(struct queue_t *q)
{
        /* TODO: return a pcb whose prioprity is the highest
         * in the queue [q] and remember to remove it from q
         * */
        int i, best = 0;

        if (empty(q))
        {
                return NULL;
        }
        for (i = 1; i < q->size; i++)
        {
                if (q->proc[i]->prio < q->proc[best]->prio)
                {
                        best = i;
                }
        }
        return remove_at(q, best);
}

struct pcb_t *purgequeue(struct queue_t *q, struct pcb_t *proc)
{
        /* TODO: remove a specific item from queue
         * */
        int i;

        if (q == NULL || proc == NULL)
        {
                return NULL;
        }
        for (i = 0; i < q->size; i++)
        {
                if (q->proc[i] == proc)
                {
                        return remove_at(q, i);
                }
        }
        return NULL;
}
