/*
 * Copyright (C) 2026 pdnguyen of HCMC University of Technology VNU-HCM
 */

/* Caitoa release
 * Source Code License Grant: The authors hereby grant to Licensee
 * personal permission to use and modify the Licensed Source Code
 * for the sole purpose of studying while attending the course CO2018.
 */

// #ifdef MM_PAGING
/*
 * PAGING based Memory Management
 * Memory physical module mm/mm-memphy.c
 */

#include "mm.h"
#include "mm64.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 *  MEMPHY_mv_csr - move MEMPHY cursor
 *  @mp: memphy struct
 *  @offset: offset
 */
int MEMPHY_mv_csr(struct memphy_struct *mp, addr_t offset)
{
   if (mp == NULL || mp->rdmflg || mp->maxsz <= 0)
      return -1;

   if (offset >= (addr_t)mp->maxsz)
      return -1;

   mp->cursor = (int)offset;

   return 0;
}

/*
 *  MEMPHY_seq_read - read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_seq_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL || value == NULL || mp->storage == NULL || mp->maxsz <= 0)
      return -1;

   if (mp->rdmflg || addr >= (addr_t)mp->maxsz)
      return -1; /* Not compatible mode for sequential read */
      /*rdmflg = 0 for seq and = 1 for RAM*/
   if (MEMPHY_mv_csr(mp, addr) != 0)
      return -1;

   *value = (BYTE)mp->storage[addr];

   return 0;
}

/*
 *  MEMPHY_read read MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @value: obtained value
 */
int MEMPHY_read(struct memphy_struct *mp, addr_t addr, BYTE *value)
{
   if (mp == NULL || value == NULL || mp->storage == NULL || mp->maxsz <= 0)
   {
      return -1;
   }

   if (addr >= (addr_t)mp->maxsz)
   {
      return -1;
   }

   /* rdmflg = 0 for seq and = 1 for RAM */
   if (mp->rdmflg)
   {
      *value = mp->storage[addr];
   }
   else if (MEMPHY_seq_read(mp, addr, value) != 0) /* Sequential access device */
   {
      return -1;
   }
 
   __atomic_fetch_add(&mp->nread, 1, __ATOMIC_RELAXED);
   return 0;
}

/*
 *  MEMPHY_seq_write - write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_seq_write(struct memphy_struct *mp, addr_t addr, BYTE value)
{
   if (mp == NULL || mp->storage == NULL || mp->maxsz <= 0)
      return -1;

   if (mp->rdmflg || addr >= (addr_t)mp->maxsz)
      return -1; /* Not compatible mode for sequential write */
      /*rdmflg = 0 for seq and = 1 for RAM*/
   if (MEMPHY_mv_csr(mp, addr) != 0)
      return -1;

   mp->storage[addr] = value;

   return 0;
}

/*
 *  MEMPHY_write-write MEMPHY device
 *  @mp: memphy struct
 *  @addr: address
 *  @data: written data
 */
int MEMPHY_write(struct memphy_struct *mp, addr_t addr, BYTE data)
{
   if (mp == NULL || mp->storage == NULL || mp->maxsz <= 0)
   {
      return -1;
   }

   if (addr >= (addr_t)mp->maxsz)
   {
      return -1;
   }

   if (mp->rdmflg)
   {
      mp->storage[addr] = data;
   }
   else if (MEMPHY_seq_write(mp, addr, data) != 0) /* Sequential access device */
   {
      return -1;
   }
 
   __atomic_fetch_add(&mp->nwrite, 1, __ATOMIC_RELAXED);

   return 0;
}


static inline int memphy_bit_test(const struct memphy_struct* mp, addr_t fpn)
{
    return (mp->fp_bitmap[fpn >> 3] >> (fpn & 7)) & 1;
}

static inline void memphy_bit_set(struct memphy_struct* mp, addr_t fpn)
{
    mp->fp_bitmap[fpn >> 3] |= (unsigned char)(1u << (fpn & 7));
}

static inline void memphy_bit_clr(struct memphy_struct* mp, addr_t fpn)
{
    mp->fp_bitmap[fpn >> 3] &= (unsigned char)~(1u << (fpn & 7));
}

/* đưa thiết bị về trạng thái "không có frame" (dùng khi format lỗi) */
static void memphy_reset_frames(struct memphy_struct* mp)
{
    mp->free_fp_list = NULL;
    mp->used_fp_list = NULL;
    free(mp->fp_bitmap);
    mp->fp_bitmap = NULL;
    mp->numfp = 0;
    mp->nfree = 0;
}

/*
 *  MEMPHY_format-format MEMPHY device
 *  @mp: memphy struct
 */
int MEMPHY_format(struct memphy_struct *mp, int pagesz)
{
   struct framephy_struct *head = NULL;
   struct framephy_struct *tail = NULL;
   addr_t numfp;
   addr_t iter;

   if (mp == NULL || pagesz <= 0 || mp->maxsz < 0)
      return -1;

   /* Formatting starts with no allocated frame.  A device smaller than one
    * page (including a disabled, zero-sized swap device) is valid, but has no
    * frame to offer. */
   mp->free_fp_list = NULL;
   mp->used_fp_list = NULL;
   mp->fp_bitmap = NULL;
   mp->numfp = 0;
   mp->nfree = 0;
   numfp = mp->maxsz / pagesz;
   if (numfp == 0)
       return 0;
   mp->fp_bitmap = calloc((size_t)((numfp + 7) / 8), 1);   /* 0 = FREE */
   if (mp->fp_bitmap == NULL)
       return -1;

   for (iter = 0; iter < numfp; iter++)
   {
      struct framephy_struct *frame = calloc(1, sizeof(*frame));

      if (frame == NULL)
      {
         while (head != NULL)
         {
            struct framephy_struct *next = head->fp_next;
            free(head);
            head = next;
         }
         memphy_reset_frames(mp);
         return -1;
      }

      frame->fpn = (addr_t)iter;
      frame->owner = NULL;

      if (tail == NULL)
         head = frame;
      else
         tail->fp_next = frame;
      tail = frame;
   }

   mp->free_fp_list = head;
   mp->numfp = numfp;
   mp->nfree = numfp;
   return 0;
}

int MEMPHY_get_freefp(struct memphy_struct *mp, addr_t *retfpn)
{
   struct framephy_struct *fp;

   if (mp == NULL || retfpn == NULL || mp->free_fp_list == NULL)
      return -1;

   fp = mp->free_fp_list;
   if (fp->fpn >= mp->numfp || memphy_bit_test(mp, fp->fpn))
       return -1;
   mp->free_fp_list = fp->fp_next;

   /* Keep the allocated frame node for ownership tracking and cleanup. */
   fp->fp_next = mp->used_fp_list;
   mp->used_fp_list = fp;
   memphy_bit_set(mp, fp->fpn);
   mp->nfree--;
   *retfpn = fp->fpn;

   return 0;
}

int MEMPHY_dump(struct memphy_struct *mp)
{
  /*
   * TODO dump memphy contnt mp->storage
   *     for tracing the memory content
   */

   return 0;
}

int MEMPHY_put_freefp(struct memphy_struct *mp, addr_t fpn)
{
   struct framephy_struct *frame;
   struct framephy_struct *previous = NULL;

   if (mp == NULL || fpn >= mp->numfp || !memphy_bit_test(mp, fpn))
       return -1;

   frame = mp->used_fp_list;
   while (frame != NULL && frame->fpn != fpn)
   {
      previous = frame;
      frame = frame->fp_next;
   }

   /* A frame not present in the used list is invalid or already free. */
   if (frame == NULL)
      return -1;

   if (previous == NULL)
      mp->used_fp_list = frame->fp_next;
   else
      previous->fp_next = frame->fp_next;

   frame->owner = NULL;
   frame->fp_next = mp->free_fp_list;
   mp->free_fp_list = frame;
   memphy_bit_clr(mp, fpn);
   mp->nfree++;

   return 0;
}

/*
 * MEMPHY_get_contig_fp - giữ chỗ nframes FPN liên tiếp [first, first+n)
 *   1. Quét bitmap theo FPN tăng dần, lấy dải FREE dài >= n đầu tiên.
 *   2. Duyệt free list 1 lần, chuyển đúng các node trong dải sang used list.
 *   3. Chỉ set bit khi đã gom đủ n node; thiếu -> trả node về, -1 (B5).
 *   Không reclaim / retry / compaction / map / xóa 0 (việc của bên gọi).
 *   Bên gọi giữ mmvm_lock.
 */
int MEMPHY_get_contig_fp(struct memphy_struct* mp, addr_t nframes, addr_t* first_fpn)
{
    struct framephy_struct** pp, * taken = NULL, * tail = NULL;
    addr_t fpn, run = 0, first = 0, got = 0;
    int found = 0;

    if (mp == NULL || first_fpn == NULL || nframes == 0 || nframes > mp->nfree)
        return -1;

    for (fpn = 0; fpn < mp->numfp; fpn++)
    {
        if (memphy_bit_test(mp, fpn))
        {
            run = 0;
            continue;
        }
        if (run == 0)
            first = fpn;
        if (++run == nframes)
        {
            found = 1;
            break;
        }
    }
    if (!found)
        return -1;                                  

    
    pp = &mp->free_fp_list;
    while (*pp != NULL && got < nframes)
    {
        struct framephy_struct* nd = *pp;
        if (nd->fpn >= first && nd->fpn < first + nframes)
        {
            *pp = nd->fp_next;
            nd->fp_next = NULL;
            if (tail == NULL)
                taken = nd;
            else
                tail->fp_next = nd;
            tail = nd;
            got++;
        }
        else
            pp = &nd->fp_next;
    }

    if (got != nframes)
    {
        if (tail != NULL)
        {
            tail->fp_next = mp->free_fp_list;
            mp->free_fp_list = taken;
        }
        return -1;
    }

    tail->fp_next = mp->used_fp_list;
    mp->used_fp_list = taken;
    for (fpn = first; fpn < first + nframes; fpn++)
        memphy_bit_set(mp, fpn);
    mp->nfree -= nframes;

    *first_fpn = first;
    return 0;
}

/*
 * MEMPHY_put_contig_fp - trả nguyên dải [first, first+n) đã cấp bởi get_contig
 *   Pre: đã unmap toàn dải. Kiểm trước (biên, tràn số, cả n bit = 1):
 *   sai -> -1, không đổi gì. Lệch giữa chừng (bit = 1 nhưng thiếu node ở
 *   used list) -> dừng, -1; frame đã trả không lấy lại (dải đã unmap).
 *   Bên gọi giữ mmvm_lock.
 */
int MEMPHY_put_contig_fp(struct memphy_struct* mp, addr_t first_fpn, addr_t nframes)
{
    addr_t fpn;

    if (mp == NULL || nframes == 0 || first_fpn >= mp->numfp ||
        nframes > mp->numfp - first_fpn)
        return -1;
    for (fpn = first_fpn; fpn < first_fpn + nframes; fpn++)
        if (!memphy_bit_test(mp, fpn))
            return -1;                                /* có frame đang FREE */

    for (fpn = first_fpn; fpn < first_fpn + nframes; fpn++)
        if (MEMPHY_put_freefp(mp, fpn) != 0)
            return -1;
    return 0;
}

/* 1 = FREE, 0 = ALLOCATED, -1 = tham số sai. Chỉ đọc. */
int MEMPHY_frame_is_free(struct memphy_struct* mp, addr_t fpn)
{
    if (mp == NULL || fpn >= mp->numfp)
        return -1;
    return memphy_bit_test(mp, fpn) ? 0 : 1;
}

/*
 * MEMPHY_verify - đếm vi phạm B1..B3 (0 = sạch, -1 = mp NULL). Chỉ đọc.
 *   fpn ngoài biên, fpn xuất hiện 2 lần, node lệch bit, fpn thiếu node,
 *   nfree khác độ dài free list.
 */
long MEMPHY_verify(struct memphy_struct* mp)
{
    struct framephy_struct* nd;
    unsigned char* seen;
    addr_t fpn, nfree_list = 0;
    long bad = 0;

    if (mp == NULL)
        return -1;
    if (mp->numfp == 0)
        return (mp->free_fp_list != NULL) + (mp->used_fp_list != NULL) + (mp->nfree != 0);

    seen = calloc((size_t)mp->numfp, 1);
    if (seen == NULL)
        return -1;

    for (nd = mp->free_fp_list; nd != NULL; nd = nd->fp_next)
    {
        nfree_list++;
        if (nd->fpn >= mp->numfp) { bad++; continue; }
        if (seen[nd->fpn]++) bad++;                  /* trùng */
        if (memphy_bit_test(mp, nd->fpn)) bad++;     /* free list nhưng bit 1 */
    }
    for (nd = mp->used_fp_list; nd != NULL; nd = nd->fp_next)
    {
        if (nd->fpn >= mp->numfp) { bad++; continue; }
        if (seen[nd->fpn]++) bad++;
        if (!memphy_bit_test(mp, nd->fpn)) bad++;    /* used list nhưng bit 0 */
    }
    for (fpn = 0; fpn < mp->numfp; fpn++)
        if (!seen[fpn]) bad++;                       /* fpn không có node */
    if (nfree_list != mp->nfree) bad++;

    free(seen);
    return bad;
}

/*
 *  Init MEMPHY struct
 */
int init_memphy(struct memphy_struct *mp, addr_t max_size, int randomflg)
{
   int format_status;

   if (mp == NULL)
      return -1;

   mp->storage = NULL;
   mp->maxsz = 0;
   mp->rdmflg = (randomflg != 0) ? 1 : 0;
   mp->cursor = 0;
   mp->free_fp_list = NULL;
   mp->used_fp_list = NULL;
   mp->fp_bitmap = NULL;
   mp->numfp = 0;
   mp->nfree = 0;
   mp->nread = 0;
   mp->nwrite= 0;
   /* maxsz is currently an int field, so reject values it cannot represent. */
   if (max_size > (addr_t)INT_MAX)
      return -1;

   mp->maxsz = (int)max_size;
   if (max_size > 0)
   {
      mp->storage = calloc((size_t)max_size, sizeof(BYTE));
      if (mp->storage == NULL)
      {
         mp->maxsz = 0;
         return -1;
      }
   }

#ifdef MM64
   format_status = MEMPHY_format(mp, PAGING64_PAGESZ);
#else
   format_status = MEMPHY_format(mp, PAGING_PAGESZ);
#endif

   if (format_status != 0)
   {
      free(mp->storage);
      mp->storage = NULL;
      mp->maxsz = 0;
      return -1;
   }

   return 0;
}

// #endif
