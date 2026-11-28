/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 *
 * demacOprx release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 *
 * system bus - Only for dma_testbech simulation access
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include "sysbus.h"
#include  "pthread.h"

#include "common.h"
#include "os-mm.h"

// Maximum number of memory banks supported
#define MAX_MEMORY_BANKS 8

// Memory bank structure
typedef struct {
    uint8_t* data;
    size_t size;
    int initialized;
} memory_bank_t_struct;

// Array of memory banks
static memory_bank_t_struct g_banks[MAX_MEMORY_BANKS] = {0};
static int g_bank_count = 0;

// Initialize a memory bank with given size in bytes
memory_bank_t memory_bank_init(size_t size_bytes) {
    // Find first free slot
    int free_slot = -1;
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (!g_banks[i].initialized) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == -1) {
        printf("Maximum number of memory banks (%d) reached\n", MAX_MEMORY_BANKS);
        return MEMORY_BANK_INVALID;
    }

    if (size_bytes == 0) {
        printf("Invalid bank size: 0\n");
        return MEMORY_BANK_INVALID;
    }

    uint8_t* data = (uint8_t*)malloc(size_bytes);
    if (!data) {
        printf("Failed to allocate memory bank of %zu bytes\n", size_bytes);
        return MEMORY_BANK_INVALID;
    }

    memset(data, 0, size_bytes);
    g_banks[free_slot].data = data;
    g_banks[free_slot].size = size_bytes;
    g_banks[free_slot].initialized = 1;
    g_bank_count++;

    printf("Memory bank [%d] initialized: %zu bytes at address %p\n", free_slot, size_bytes, (void*)data);
    return free_slot;
}

// Cleanup memory bank
void memory_bank_cleanup(void) {
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (g_banks[i].initialized) {
            if (g_banks[i].data) {
                free(g_banks[i].data);
                g_banks[i].data = NULL;
            }
            g_banks[i].size = 0;
            g_banks[i].initialized = 0;
            g_bank_count--;
            printf("Memory bank [%d] cleaned up\n", i);
        }
    }
}

int memory_bank_is_available(void) {
    // Check if at least one memory bank is initialized
    for (int i = 0; i < MAX_MEMORY_BANKS; i++) {
        if (g_banks[i].initialized) {
            return 1;
        }
    }
    return 0;
}

// Get pointer to locate data at offset
void* memory_bank_get_ptr(uint64_t offset) {
    if (offset >= g_banks[0].size) {
        return NULL;
    }

    return g_banks[0].data + offset;
}

// Get bank size in bytes
size_t memory_bank_get_size() {
    return g_banks[0].size;
}


#ifdef __cplusplus
extern "C" {
#endif

/**
 * DPI Callback: Called from RTL when DMA read access
 */
/*
int dpi_bank_read32_legacy(long long offset) {
    uint8_t data[4] = {0};

    void* src = memory_bank_get_ptr((size_t)offset);

    //printf("MEM READ RESULT: Address=%p, DPI return=%x, Readback=%x, Match=%d\n",
    //        (void*)((size_t)offset), *(unsigned int*)data, *(unsigned int*)data, 1);
    //fflush(stdout);
    
    memcpy(&data, src, 4);

    return  *((uint32_t*)data);
}
*/
/**
 * DPI Callback: Called from RTL when DMA write access
 */
/*
int dpi_bank_write32_legacy(long long offset, int value) {
    void* dst = memory_bank_get_ptr((size_t)offset);
    //printf("MEM WRITE RESULT: Address=%p, Write data=%x, Write rc=0, Readback=%x, Match=1\n",
    //        (void*)((size_t)offset), *(unsigned int*)&value, *(unsigned int*)&value);
    memcpy(dst,  &value, 4);
    //fflush(stdout);
       
    return 0;
}
    */
   
   static struct memphy_struct *g_os_mram = NULL;

void sysbus_set_os_mram(struct memphy_struct *mram) {
    g_os_mram = mram;
}

static void* sysbus_resolve_os_ptr(uint64_t offset, size_t access_len) {
    if (g_os_mram == NULL || g_os_mram->storage == NULL || g_os_mram->maxsz <= 0) {
        return NULL;
    }
    uint64_t maxsz = (uint64_t)g_os_mram->maxsz;
    if (offset >= maxsz || access_len > maxsz - offset) {
        return NULL;
    }
    return (void *)(g_os_mram->storage + offset);
}

int dpi_bank_read32_legacy(long long offset) {
    uint8_t data[4] = {0};

    if (offset < 0) {
        return 0;
    }

    void* src = memory_bank_get_ptr((size_t)offset);
    if (!src) {
        src = sysbus_resolve_os_ptr((uint64_t)offset, 4);
    }
    if (!src) {
        return 0;
    }

    memcpy(data, src, 4);
    return *((uint32_t*)data);
}

int dpi_bank_write32_legacy(long long offset, int value) {
    if (offset < 0) {
        return -1;
    }

    void* dst = memory_bank_get_ptr((size_t)offset);
    if (!dst) {
        dst = sysbus_resolve_os_ptr((uint64_t)offset, 4);
    }
    if (!dst) {
        return -1;
    }

    memcpy(dst, &value, 4);
    return 0;
}
/**
 * DPI Callback: Called from RTL when the DMA channel raises ch_done.
 * This is the interrupt line of the device. The interrupt is handled
 * once, in the notifier thread, by dma_irq_handler().
 */
void dma_done_callback(void)
{
}
/**
 * dma_irq_handler - interrupt handler for one finished DMA job
 * Runs in the notifier thread. The job information is passed in, so the
 * handler reports the job that finished, not the latest one submitted.
 */
void dma_irq_handler(uint32_t pid, uint64_t dst_off, uint64_t len,
                     struct memphy_struct *mram, int use_os_mram)
{
    const uint8_t *pdst = NULL;
    uint64_t max_len = 0;

    printf("DMA completed: pid=%u\n", pid);

    if (memory_bank_is_available()) 
    {
        pdst = (const uint8_t *)memory_bank_get_ptr(dst_off);
        max_len = len;
    } 
    else if (use_os_mram && mram != NULL && mram->storage != NULL && mram->maxsz > 0) 
    {
        uint64_t maxsz = (uint64_t)mram->maxsz;
        if (dst_off < maxsz) 
        {
            pdst = (const uint8_t *)(mram->storage + dst_off);
            max_len = (len < maxsz - dst_off) ? len : (maxsz - dst_off);
        }
    }

    if (pdst != NULL && max_len > 0) 
    {
        uint64_t show = (max_len < 8) ? max_len : 8;
        printf("  dst @ %llu (len=%llu): ",
               (unsigned long long)dst_off,
               (unsigned long long)len);
        for (uint64_t i = 0; i < show; i++) 
        {
            printf("0x%02x%s", pdst[i], (i + 1 < show) ? " " : "");
        }
        printf("\n");
    }
    fflush(stdout)    
    /* Wake up the process that issued this job */
    if (g_dma_irq_hook != NULL)
    {
        g_dma_irq_hook(pid);
    }
}

/**
 * DPI Callback: Called from RTL when DMA transfer completes
 */
/*
void dma_done_callback(void)
{
    printf("DMA completed\n");
    fflush(stdout);   // keep ordering vs. printf_IMPORTANT output
}
*/
#ifdef __cplusplus
}
#endif

