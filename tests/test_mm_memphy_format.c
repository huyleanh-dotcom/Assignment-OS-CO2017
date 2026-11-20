#include "mm.h"
#include "mm64.h"

#include <assert.h>
#include <limits.h>
#include <stdlib.h>

static void free_frame_list(struct framephy_struct *frame)
{
  while (frame != NULL)
    {
      struct framephy_struct *next = frame->fp_next;
      free(frame);
      frame = next;
    }
}

static void check_formatted_device(int size, int expected_frames)
{
  struct memphy_struct memphy = {0};
  struct framephy_struct *frame;
  int frame_count = 0;

  memphy.maxsz = size;
  assert(MEMPHY_format(&memphy, PAGING64_PAGESZ) == 0);
  assert(memphy.used_fp_list == NULL);

  for (frame = memphy.free_fp_list; frame != NULL; frame = frame->fp_next)
    {
      assert(frame->fpn == (addr_t)frame_count);
      assert(frame->owner == NULL);
      frame_count++;
    }

  assert(frame_count == expected_frames);
  free_frame_list(memphy.free_fp_list);
}

static void check_get_free_frame(void)
{
  struct memphy_struct memphy = {0};
  struct framephy_struct *first_free;
  addr_t fpn = 99;

  memphy.maxsz = 3 * PAGING64_PAGESZ;
  assert(MEMPHY_format(&memphy, PAGING64_PAGESZ) == 0);

  first_free = memphy.free_fp_list;
  assert(MEMPHY_get_freefp(&memphy, &fpn) == 0);
  assert(fpn == 0);
  assert(memphy.used_fp_list == first_free);
  assert(memphy.used_fp_list->fpn == 0);
  assert(memphy.free_fp_list->fpn == 1);

  assert(MEMPHY_get_freefp(&memphy, &fpn) == 0);
  assert(fpn == 1);
  assert(memphy.used_fp_list->fpn == 1);

  assert(MEMPHY_get_freefp(&memphy, &fpn) == 0);
  assert(fpn == 2);
  assert(memphy.free_fp_list == NULL);
  assert(memphy.used_fp_list->fpn == 2);

  fpn = 99;
  assert(MEMPHY_get_freefp(&memphy, &fpn) == -1);
  assert(fpn == 99);
  assert(MEMPHY_get_freefp(NULL, &fpn) == -1);
  assert(MEMPHY_get_freefp(&memphy, NULL) == -1);

  free_frame_list(memphy.used_fp_list);
}

static void check_put_free_frame(void)
{
  struct memphy_struct memphy = {0};
  struct mm_struct owner = {0};
  addr_t fpn;

  memphy.maxsz = 3 * PAGING64_PAGESZ;
  assert(MEMPHY_format(&memphy, PAGING64_PAGESZ) == 0);
  assert(MEMPHY_get_freefp(&memphy, &fpn) == 0);
  assert(fpn == 0);
  assert(MEMPHY_get_freefp(&memphy, &fpn) == 0);
  assert(fpn == 1);

  memphy.used_fp_list->owner = &owner;
  assert(MEMPHY_put_freefp(&memphy, 1) == 0);
  assert(memphy.free_fp_list->fpn == 1);
  assert(memphy.free_fp_list->owner == NULL);
  assert(memphy.used_fp_list->fpn == 0);

  assert(MEMPHY_put_freefp(&memphy, 1) == -1);
  assert(MEMPHY_put_freefp(&memphy, 99) == -1);
  assert(MEMPHY_put_freefp(NULL, 0) == -1);

  assert(MEMPHY_get_freefp(&memphy, &fpn) == 0);
  assert(fpn == 1);

  assert(MEMPHY_put_freefp(&memphy, 0) == 0);
  assert(memphy.used_fp_list->fpn == 1);
  assert(memphy.free_fp_list->fpn == 0);

  free_frame_list(memphy.free_fp_list);
  free_frame_list(memphy.used_fp_list);
}

static void check_random_read(void)
{
  BYTE storage[] = {11, 22, 33};
  struct memphy_struct memphy = {0};
  BYTE value = 99;

  memphy.storage = storage;
  memphy.maxsz = (int)sizeof(storage);
  memphy.rdmflg = 1;

  assert(MEMPHY_read(&memphy, 0, &value) == 0);
  assert(value == 11);
  assert(MEMPHY_read(&memphy, 2, &value) == 0);
  assert(value == 33);

  value = 99;
  assert(MEMPHY_read(&memphy, 3, &value) == -1);
  assert(value == 99);
  assert(MEMPHY_read(&memphy, (addr_t)-1, &value) == -1);
  assert(value == 99);
  assert(MEMPHY_read(NULL, 0, &value) == -1);
  assert(MEMPHY_read(&memphy, 0, NULL) == -1);

  memphy.storage = NULL;
  assert(MEMPHY_read(&memphy, 0, &value) == -1);
  memphy.storage = storage;
  memphy.maxsz = 0;
  assert(MEMPHY_read(&memphy, 0, &value) == -1);
}

static void check_random_write(void)
{
  BYTE storage[] = {11, 22, 33};
  struct memphy_struct memphy = {0};

  memphy.storage = storage;
  memphy.maxsz = (int)sizeof(storage);
  memphy.rdmflg = 1;

  assert(MEMPHY_write(&memphy, 0, 44) == 0);
  assert(storage[0] == 44);
  assert(MEMPHY_write(&memphy, 2, 55) == 0);
  assert(storage[2] == 55);

  assert(MEMPHY_write(&memphy, 3, 66) == -1);
  assert(storage[0] == 44 && storage[1] == 22 && storage[2] == 55);
  assert(MEMPHY_write(&memphy, (addr_t)-1, 66) == -1);
  assert(storage[0] == 44 && storage[1] == 22 && storage[2] == 55);
  assert(MEMPHY_write(NULL, 0, 66) == -1);

  memphy.storage = NULL;
  assert(MEMPHY_write(&memphy, 0, 66) == -1);
  memphy.storage = storage;
  memphy.maxsz = 0;
  assert(MEMPHY_write(&memphy, 0, 66) == -1);
  assert(storage[0] == 44 && storage[1] == 22 && storage[2] == 55);
}

static void check_init_memphy(void)
{
  struct memphy_struct memphy;
  int index;

  assert(init_memphy(NULL, PAGING64_PAGESZ, 1) == -1);

  assert(init_memphy(&memphy, 0, 1) == 0);
  assert(memphy.storage == NULL);
  assert(memphy.maxsz == 0);
  assert(memphy.rdmflg == 1);
  assert(memphy.cursor == 0);
  assert(memphy.free_fp_list == NULL);
  assert(memphy.used_fp_list == NULL);

  assert(init_memphy(&memphy, PAGING64_PAGESZ - 1, 0) == 0);
  assert(memphy.storage != NULL);
  assert(memphy.maxsz == PAGING64_PAGESZ - 1);
  assert(memphy.rdmflg == 0);
  assert(memphy.cursor == 0);
  assert(memphy.free_fp_list == NULL);
  for (index = 0; index < memphy.maxsz; index++)
    assert(memphy.storage[index] == 0);
  free(memphy.storage);

  assert(init_memphy(&memphy, PAGING64_PAGESZ, 7) == 0);
  assert(memphy.storage != NULL);
  assert(memphy.rdmflg == 1);
  assert(memphy.free_fp_list != NULL);
  assert(memphy.free_fp_list->fpn == 0);
  assert(memphy.free_fp_list->fp_next == NULL);
  free(memphy.storage);
  free_frame_list(memphy.free_fp_list);

  assert(init_memphy(&memphy, (addr_t)INT_MAX + 1, 1) == -1);
  assert(memphy.storage == NULL);
  assert(memphy.maxsz == 0);
  assert(memphy.free_fp_list == NULL);
  assert(memphy.used_fp_list == NULL);
}

static void check_sequential_read(void)
{
  BYTE storage[] = {11, 22, 33, 44};
  struct memphy_struct memphy = {0};
  BYTE value = 99;

  memphy.storage = storage;
  memphy.maxsz = (int)sizeof(storage);
  memphy.rdmflg = 0;
  memphy.cursor = 3;

  assert(MEMPHY_seq_read(&memphy, 0, &value) == 0);
  assert(value == 11);
  assert(memphy.cursor == 0);
  assert(MEMPHY_seq_read(&memphy, 3, &value) == 0);
  assert(value == 44);
  assert(memphy.cursor == 3);

  value = 99;
  assert(MEMPHY_seq_read(&memphy, 4, &value) == -1);
  assert(value == 99);
  assert(memphy.cursor == 3);
  assert(MEMPHY_seq_read(&memphy, (addr_t)-1, &value) == -1);
  assert(value == 99);
  assert(MEMPHY_seq_read(NULL, 0, &value) == -1);
  assert(MEMPHY_seq_read(&memphy, 0, NULL) == -1);

  memphy.rdmflg = 1;
  assert(MEMPHY_seq_read(&memphy, 0, &value) == -1);
  memphy.rdmflg = 0;
  memphy.storage = NULL;
  assert(MEMPHY_seq_read(&memphy, 0, &value) == -1);
}

static void check_sequential_write(void)
{
  BYTE storage[] = {11, 22, 33, 44};
  struct memphy_struct memphy = {0};

  memphy.storage = storage;
  memphy.maxsz = (int)sizeof(storage);
  memphy.rdmflg = 0;
  memphy.cursor = 3;

  assert(MEMPHY_seq_write(&memphy, 0, 55) == 0);
  assert(storage[0] == 55);
  assert(memphy.cursor == 0);
  assert(MEMPHY_seq_write(&memphy, 3, 66) == 0);
  assert(storage[3] == 66);
  assert(memphy.cursor == 3);

  assert(MEMPHY_seq_write(&memphy, 4, 77) == -1);
  assert(storage[0] == 55 && storage[1] == 22
         && storage[2] == 33 && storage[3] == 66);
  assert(memphy.cursor == 3);
  assert(MEMPHY_seq_write(&memphy, (addr_t)-1, 77) == -1);
  assert(MEMPHY_seq_write(NULL, 0, 77) == -1);

  memphy.rdmflg = 1;
  assert(MEMPHY_seq_write(&memphy, 0, 77) == -1);
  assert(storage[0] == 55);
  memphy.rdmflg = 0;
  memphy.storage = NULL;
  assert(MEMPHY_seq_write(&memphy, 0, 77) == -1);
}

static void check_move_cursor(void)
{
  struct memphy_struct memphy = {0};

  memphy.maxsz = 4;
  memphy.rdmflg = 0;
  memphy.cursor = 2;

  assert(MEMPHY_mv_csr(&memphy, 0) == 0);
  assert(memphy.cursor == 0);
  assert(MEMPHY_mv_csr(&memphy, 3) == 0);
  assert(memphy.cursor == 3);

  assert(MEMPHY_mv_csr(&memphy, 4) == -1);
  assert(memphy.cursor == 3);
  assert(MEMPHY_mv_csr(&memphy, (addr_t)-1) == -1);
  assert(memphy.cursor == 3);
  assert(MEMPHY_mv_csr(NULL, 0) == -1);

  memphy.rdmflg = 1;
  assert(MEMPHY_mv_csr(&memphy, 0) == -1);
  assert(memphy.cursor == 3);
  memphy.rdmflg = 0;
  memphy.maxsz = 0;
  assert(MEMPHY_mv_csr(&memphy, 0) == -1);
}

static void check_swap_copy_page(void)
{
  struct memphy_struct source;
  struct memphy_struct destination;
  int index;

  assert(init_memphy(&source, 2 * PAGING64_PAGESZ, 1) == 0);
  assert(init_memphy(&destination, 2 * PAGING64_PAGESZ, 1) == 0);

  for (index = 0; index < PAGING64_PAGESZ; index++)
    {
      source.storage[PAGING64_PAGESZ + index] = (BYTE)(index % 101);
      destination.storage[index] = (BYTE)-1;
    }

  assert(__swap_cp_page(&source, 1, &destination, 0) == 0);
  for (index = 0; index < PAGING64_PAGESZ; index++)
    assert(destination.storage[index] == (BYTE)(index % 101));

  destination.storage[PAGING64_PAGESZ] = 77;
  assert(__swap_cp_page(&source, 2, &destination, 1) == -1);
  assert(destination.storage[PAGING64_PAGESZ] == 77);
  assert(__swap_cp_page(&source, 0, &destination, 2) == -1);
  assert(__swap_cp_page(NULL, 0, &destination, 0) == -1);
  assert(__swap_cp_page(&source, 0, NULL, 0) == -1);

  free(source.storage);
  free_frame_list(source.free_fp_list);
  free_frame_list(source.used_fp_list);
  free(destination.storage);
  free_frame_list(destination.free_fp_list);
  free_frame_list(destination.used_fp_list);

  assert(init_memphy(&source, PAGING64_PAGESZ - 1, 1) == 0);
  assert(init_memphy(&destination, PAGING64_PAGESZ, 1) == 0);
  assert(__swap_cp_page(&source, 0, &destination, 0) == -1);
  free(source.storage);
  free(destination.storage);
  free_frame_list(destination.free_fp_list);
}

int main(void)
{
  struct memphy_struct memphy = {0};

  assert(MEMPHY_format(NULL, PAGING64_PAGESZ) == -1);
  assert(MEMPHY_format(&memphy, 0) == -1);

  check_formatted_device(0, 0);
  check_formatted_device(PAGING64_PAGESZ - 1, 0);
  check_formatted_device(PAGING64_PAGESZ, 1);
  check_formatted_device(3 * PAGING64_PAGESZ + 17, 3);
  check_get_free_frame();
  check_put_free_frame();
  check_random_read();
  check_random_write();
  check_init_memphy();
  check_sequential_read();
  check_sequential_write();
  check_move_cursor();
  check_swap_copy_page();

  return 0;
}
