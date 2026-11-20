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
      return -1;

   if (addr >= (addr_t)mp->maxsz)
      return -1;

   /* rdmflg = 0 for seq and = 1 for RAM */
   if (mp->rdmflg)
      *value = mp->storage[addr];
   else /* Sequential access device */
      return MEMPHY_seq_read(mp, addr, value);

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
      return -1;

   if (addr >= (addr_t)mp->maxsz)
      return -1;

   if (mp->rdmflg)
      mp->storage[addr] = data;
   else /* Sequential access device */
      return MEMPHY_seq_write(mp, addr, data);

   return 0;
}

/*
 *  MEMPHY_format-format MEMPHY device
 *  @mp: memphy struct
 */
int MEMPHY_format(struct memphy_struct *mp, int pagesz)
{
   struct framephy_struct *head = NULL;
   struct framephy_struct *tail = NULL;
   int numfp;
   int iter;

   if (mp == NULL || pagesz <= 0 || mp->maxsz < 0)
      return -1;

   /* Formatting starts with no allocated frame.  A device smaller than one
    * page (including a disabled, zero-sized swap device) is valid, but has no
    * frame to offer. */
   mp->free_fp_list = NULL;
   mp->used_fp_list = NULL;
   numfp = mp->maxsz / pagesz;

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

   return 0;
}

int MEMPHY_get_freefp(struct memphy_struct *mp, addr_t *retfpn)
{
   struct framephy_struct *fp;

   if (mp == NULL || retfpn == NULL || mp->free_fp_list == NULL)
      return -1;

   fp = mp->free_fp_list;
   mp->free_fp_list = fp->fp_next;

   /* Keep the allocated frame node for ownership tracking and cleanup. */
   fp->fp_next = mp->used_fp_list;
   mp->used_fp_list = fp;
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

   if (mp == NULL)
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

   return 0;
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
