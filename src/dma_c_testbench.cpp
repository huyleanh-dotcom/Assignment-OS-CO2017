/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * DMA C Testbench
 */

/**
 * Build modes:
 *   DMA_MAIN=1 - Build as standalone executable with main()
 *   DMA_MAIN=0 - Build as library module with do_dma()
 */

#ifndef DMA_MAIN
#define DMA_MAIN 0
#endif

#include "sysbus.h"
#include "dpi_rtl.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <cstdbool>
#include <thread>
#include <atomic>
#include <mutex>
#include <pthread.h>

// Generated DPI functions from RTL
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vdma_top.h"
#include "Vdma_top__Dpi.h"

extern "C" {
#include "common.h"
#include "os-mm.h"
}

static std::mutex g_rtl_dma_mtx;

// RTL Direct Test - Drive Verilator RTL directly
extern "C" int dpi_rtl_dma_copy(uint64_t src_offset, uint64_t dst_offset, uint64_t length);
extern "C" int dpi_rtl_dma_is_available(void);

#define DMA_QUEUE_CAP 64

typedef struct {
    uint64_t src;
    uint64_t dst;
    uint64_t length;
    struct memphy_struct *mram;
    int use_os_mram;
    int status;
} dma_job_t;

typedef struct {
    dma_job_t jobs[DMA_QUEUE_CAP];
    int head;
    int tail;
    int count;
    int stop;
    int started;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    pthread_t worker;
} dma_queue_t;

/* Thread 1: Độc lập thực thi quá trình copy RTL qua Verilator */
static dma_queue_t g_dma_engine = {
    .head = 0,
    .tail = 0,
    .count = 0,
    .stop = 0,
    .started = 0,
    .mtx = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER
};

/* Thread 2: Độc lập phát thông báo hoàn tất (dma_done_callback) */
static dma_queue_t g_dma_notifier = {
    .head = 0,
    .tail = 0,
    .count = 0,
    .stop = 0,
    .started = 0,
    .mtx = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER
};

static void dma_dump_8bytes(const char *label, const dma_job_t *job) {
    const uint8_t *psrc = NULL;
    const uint8_t *pdst = NULL;
    uint64_t avail_src = 0;
    uint64_t avail_dst = 0;

    if (memory_bank_is_available()) {
        psrc = (const uint8_t *)memory_bank_get_ptr(job->src);
        pdst = (const uint8_t *)memory_bank_get_ptr(job->dst);
        avail_src = (job->length < 8) ? job->length : 8;
        avail_dst = (job->length < 8) ? job->length : 8;
    } else if (job->use_os_mram && job->mram != NULL &&
               job->mram->storage != NULL && job->mram->maxsz > 0) {
        uint64_t maxsz = (uint64_t)job->mram->maxsz;
        if (job->src < maxsz) {
            psrc = (const uint8_t *)(job->mram->storage + job->src);
            uint64_t rem = maxsz - job->src;
            uint64_t lim = (job->length < rem) ? job->length : rem;
            avail_src = (lim < 8) ? lim : 8;
        }
        if (job->dst < maxsz) {
            pdst = (const uint8_t *)(job->mram->storage + job->dst);
            uint64_t rem = maxsz - job->dst;
            uint64_t lim = (job->length < rem) ? job->length : rem;
            avail_dst = (lim < 8) ? lim : 8;
        }
    }

    uint8_t s[8] = {0};
    uint8_t d[8] = {0};
    if (psrc != NULL) {
        for (uint64_t i = 0; i < avail_src; i++) s[i] = psrc[i];
    }
    if (pdst != NULL) {
        for (uint64_t i = 0; i < avail_dst; i++) d[i] = pdst[i];
    }

    printf("%s:\n", label);
    printf("  src[0] = 0x%02x, src[1] = 0x%02x\n"
           "  src[2] = 0x%02x, src[3] = 0x%02x\n"
           "  src[4] = 0x%02x, src[5] = 0x%02x\n"
           "  src[6] = 0x%02x, src[7] = 0x%02x\n",
           s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7]);
    printf("  dst[0] = 0x%02x, dst[1] = 0x%02x\n"
           "  dst[2] = 0x%02x, dst[3] = 0x%02x\n"
           "  dst[4] = 0x%02x, dst[5] = 0x%02x\n"
           "  dst[6] = 0x%02x, dst[7] = 0x%02x\n",
           d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
    fflush(stdout);
}

static int dma_queue_enqueue(dma_queue_t *q, const dma_job_t *job) {
    pthread_mutex_lock(&q->mtx);
    if (!q->started || q->stop || q->count >= DMA_QUEUE_CAP) {
        pthread_mutex_unlock(&q->mtx);
        return -1;
    }

    q->jobs[q->tail] = *job;
    q->tail = (q->tail + 1) % DMA_QUEUE_CAP;
    q->count++;

    pthread_cond_signal(&q->cv);
    pthread_mutex_unlock(&q->mtx);
    return 0;
}

static void *dma_notifier_thread(void *arg) {
    (void)arg;

    while (1) {
        dma_job_t job;

        pthread_mutex_lock(&g_dma_notifier.mtx);
        while (!g_dma_notifier.stop && g_dma_notifier.count == 0) {
            pthread_cond_wait(&g_dma_notifier.cv, &g_dma_notifier.mtx);
        }

        if (g_dma_notifier.stop && g_dma_notifier.count == 0) {
            pthread_mutex_unlock(&g_dma_notifier.mtx);
            break;
        }

        job = g_dma_notifier.jobs[g_dma_notifier.head];
        g_dma_notifier.head = (g_dma_notifier.head + 1) % DMA_QUEUE_CAP;
        g_dma_notifier.count--;
        pthread_mutex_unlock(&g_dma_notifier.mtx);

        if (job.status == 0) {
             printf("[DMA_NOTIFIER tid=%lu] IRQ: src=%llu dst=%llu len=%llu %s\n",
               (unsigned long)pthread_self(),
               (unsigned long long)job.src,
               (unsigned long long)job.dst,
               (unsigned long long)job.length,
               job.status == 0 ? "done" : "FAILED");
        fflush(stdout);
        dma_done_callback();
        }
    }

    return NULL;
}

static void *dma_engine_thread(void *arg) {
    (void)arg;

    while (1) {
        dma_job_t job;

        pthread_mutex_lock(&g_dma_engine.mtx);
        while (!g_dma_engine.stop && g_dma_engine.count == 0) {
            pthread_cond_wait(&g_dma_engine.cv, &g_dma_engine.mtx);
        }

        if (g_dma_engine.stop && g_dma_engine.count == 0) {
            pthread_mutex_unlock(&g_dma_engine.mtx);
            break;
        }

        job = g_dma_engine.jobs[g_dma_engine.head];
        g_dma_engine.head = (g_dma_engine.head + 1) % DMA_QUEUE_CAP;
        g_dma_engine.count--;
        pthread_mutex_unlock(&g_dma_engine.mtx);

        printf("[DMA_ENGINE tid=%lu] executing RTL DMA src=%llu dst=%llu len=%llu\n",
               (unsigned long)pthread_self(),
               (unsigned long long)job.src,
               (unsigned long long)job.dst,
               (unsigned long long)job.length);
        printf("\nCalling RTL DMA...\n");
        fflush(stdout);

        int rc = -1;
        {
            std::lock_guard<std::mutex> rtl_lock(g_rtl_dma_mtx);
            if (job.use_os_mram && job.mram != NULL) {
                sysbus_set_os_mram(job.mram);
            }
            rc = dpi_rtl_dma_copy(job.src, job.dst, job.length);
        }

        printf("[DMA_ENGINE tid=%lu] finished RTL DMA rc=%d\n",
               (unsigned long)pthread_self(), rc);
        dma_dump_8bytes("\nAfter copy", &job);
        fflush(stdout);

        dma_job_t done_job = job;
        done_job.status = (rc >= 0) ? 0 : -1;
        if (done_job.status == 0) {
            dma_record_last_transfer(job.src, job.dst, job.length,
                                     job.mram, job.use_os_mram);
        if (dma_queue_enqueue(&g_dma_notifier, &done_job) != 0) {
            /* notifier queue full or stopped: report here.
             * Do NOT call dma_done_callback() by hand: it would print twice. */
            printf("[DMA_ENGINE] IRQ (direct): src=%llu dst=%llu len=%llu %s\n",
                   (unsigned long long)job.src,
                   (unsigned long long)job.dst,
                   (unsigned long long)job.length,
                   done_job.status == 0 ? "done" : "FAILED");
        }
        }
    }

    return NULL;
}

int dma_notifier_init(void) {
    pthread_mutex_lock(&g_dma_notifier.mtx);
    if (g_dma_notifier.started) {
        pthread_mutex_unlock(&g_dma_notifier.mtx);
        return 0;
    }

    g_dma_notifier.stop = 0;
    g_dma_notifier.head = 0;
    g_dma_notifier.tail = 0;
    g_dma_notifier.count = 0;

    if (pthread_create(&g_dma_notifier.worker, NULL, dma_notifier_thread, NULL) != 0) {
        pthread_mutex_unlock(&g_dma_notifier.mtx);
        return -1;
    }

    g_dma_notifier.started = 1;
    pthread_mutex_unlock(&g_dma_notifier.mtx);
    return 0;
}

void dma_notifier_shutdown(void) {
    pthread_mutex_lock(&g_dma_notifier.mtx);
    if (!g_dma_notifier.started) {
        pthread_mutex_unlock(&g_dma_notifier.mtx);
        return;
    }
    g_dma_notifier.stop = 1;
    pthread_cond_broadcast(&g_dma_notifier.cv);
    pthread_mutex_unlock(&g_dma_notifier.mtx);

    pthread_join(g_dma_notifier.worker, NULL);

    pthread_mutex_lock(&g_dma_notifier.mtx);
    g_dma_notifier.started = 0;
    pthread_mutex_unlock(&g_dma_notifier.mtx);
}

int dma_engine_init(void) {
    pthread_mutex_lock(&g_dma_engine.mtx);
    if (g_dma_engine.started) {
        pthread_mutex_unlock(&g_dma_engine.mtx);
        return 0;
    }

    g_dma_engine.stop = 0;
    g_dma_engine.head = 0;
    g_dma_engine.tail = 0;
    g_dma_engine.count = 0;

    if (pthread_create(&g_dma_engine.worker, NULL, dma_engine_thread, NULL) != 0) {
        pthread_mutex_unlock(&g_dma_engine.mtx);
        return -1;
    }

    g_dma_engine.started = 1;
    pthread_mutex_unlock(&g_dma_engine.mtx);
    return 0;
}

void dma_worker_shutdown(void) {
    pthread_mutex_lock(&g_dma_engine.mtx);
    if (!g_dma_engine.started) {
        pthread_mutex_unlock(&g_dma_engine.mtx);
        return;
    }
    g_dma_engine.stop = 1;
    pthread_cond_broadcast(&g_dma_engine.cv);
    pthread_mutex_unlock(&g_dma_engine.mtx);

    pthread_join(g_dma_engine.worker, NULL);

    pthread_mutex_lock(&g_dma_engine.mtx);
    g_dma_engine.started = 0;
    pthread_mutex_unlock(&g_dma_engine.mtx);
}

int dpi_dma_copy(struct pcb_t *caller, uint64_t src_offset, uint64_t dst_offset, uint64_t length) {
    void *src = NULL;
    void *dst = NULL;
    struct memphy_struct *mram = NULL;
    int use_os_mram = 0;

    if (memory_bank_is_available()) {
        printf("dma_testbench mode with dpi_dma_copy from memory bank\n");
        src = memory_bank_get_ptr(src_offset);
        dst = memory_bank_get_ptr(dst_offset);
    } else {
        if (caller == NULL || caller->krnl == NULL) {
            printf("Caller or krnl is NULL\n");
            return -1;
        }

        struct krnl_t *krnl = caller->krnl;
        mram = krnl->mram;

        if (mram == NULL || mram->storage == NULL || mram->maxsz <= 0) {
            printf("Mram is not initialized\n");
            return -1;
        }

        uint64_t maxsz = (uint64_t)mram->maxsz;
        if (src_offset >= maxsz || length > maxsz - src_offset ||
            dst_offset >= maxsz || length > maxsz - dst_offset) {
            printf("Out of memory\n");
            return -1;
        }

        src = (void *)(mram->storage + src_offset);
        dst = (void *)(mram->storage + dst_offset);
        use_os_mram = 1;
    }

    if (src == NULL || dst == NULL) {
        printf("Invalid src or dst pointer\n");
        return -1;
    }

    if (!g_dma_engine.started && dma_engine_init() != 0) {
        printf("Failed to init DMA engine worker thread\n");
        return -1;
    }

    if (!g_dma_notifier.started && dma_notifier_init() != 0) {
        printf("Failed to init DMA notifier thread\n");
        return -1;
    }
    /* Fill a test pattern only in standalone testbench mode.
     * In OS mode the source region holds real process data. */
    if (!use_os_mram) 
    {
        uint8_t *psrc = (uint8_t *)src;
        uint64_t pattern_length = (length <= 255) ? length : 255;
        for (uint64_t i = 0; i < length; i++) 
        {
            psrc[i] = (uint8_t)((i % pattern_length) + 1);
        }
    }

    printf("dpi_dma_copy: src=%llu, dst=%llu, len=%llu\n",
           (unsigned long long)src_offset,
           (unsigned long long)dst_offset,
           (unsigned long long)length);
    printf("[CPU tid=%lu] dpi_dma_copy enqueue: src=%llu, dst=%llu, len=%llu\n",
           (unsigned long)pthread_self(),
           (unsigned long long)src_offset,
           (unsigned long long)dst_offset,
           (unsigned long long)length);

    dma_job_t job = {
        .src = src_offset,
        .dst = dst_offset,
        .length = length,
        .mram = mram,
        .use_os_mram = use_os_mram,
        .status = 0
    };

    dma_dump_8bytes("Before copy", &job);

    if (dma_queue_enqueue(&g_dma_engine, &job) != 0) {
        printf("DMA engine queue full or stopped\n");
        return -1;
    }

    /* Trả về ngay: CPU caller đi làm việc khác, dma_engine_thread chạy độc lập */
    return 0;
}

extern "C" int do_dma(struct pcb_t *caller, uint64_t source, uint64_t destination, uint64_t length) {
    if (length == 0) {
        return 0;
    }

    int rc = dpi_dma_copy(caller, source, destination, length);
    if (rc < 0) {
        printf("Error for DMA \n");
        fflush(stdout);
        return rc;
    }

    return 0;
}

#if DMA_MAIN == 1
int main(int argc, char** argv) {
    uint64_t source = 0x1000;
    uint64_t destination = 0x2000;
    uint64_t length = 512;

    if (argc >= 4) {
        source = strtoull(argv[1], NULL, 0);
        destination = strtoull(argv[2], NULL, 0);
        length = strtoull(argv[3], NULL, 0);
    }

    printf("Running dma_testbench source=0x%llx, destination=0x%llx, length=%llu\n",
           (unsigned long long)source, (unsigned long long)destination, (unsigned long long)length);

    memory_bank_init(1024 * 1024);

    if (rtl_dma_init_simulation() != 0) {
        printf("Cannot initialize RTL simulation\n");
        dma_worker_shutdown();
        dma_notifier_shutdown();
        memory_bank_cleanup();
        return 1;
    }

    struct pcb_t *caller = (struct pcb_t *)calloc(1, sizeof(struct pcb_t));
    caller->krnl = (struct krnl_t *)calloc(1, sizeof(struct krnl_t));
    caller->krnl->mm = (struct mm_struct *)calloc(1, sizeof(struct mm_struct));
    caller->krnl->mram = (struct memphy_struct *)calloc(1, sizeof(struct memphy_struct));
    caller->krnl->active_mswp = (struct memphy_struct *)calloc(1, sizeof(struct memphy_struct));
    caller->mm = caller->krnl->mm;
    caller->pid = 0;

    int result = do_dma(caller, source, destination, length);
    printf("[CPU tid=%lu] main continued immediately after do_dma()\n",
           (unsigned long)pthread_self());
    fflush(stdout);

    dma_worker_shutdown();
    dma_notifier_shutdown();

    free(caller->krnl->active_mswp);
    free(caller->krnl->mram);
    free(caller->krnl->mm);
    free(caller->krnl);
    free(caller);

    memory_bank_cleanup();

    return result;
}
#endif
