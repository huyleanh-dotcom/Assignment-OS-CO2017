/* Exercise the same device wiring used by the OS entry point. */
#define main os_program_main
#include "../src/os.c"
#undef main

#include <assert.h>
#include "libmem.h"

static void
destroy_device (struct memphy_struct *device)
{
  struct framephy_struct *node;

  while ((node = device->free_fp_list) != NULL)
    {
      device->free_fp_list = node->fp_next;
      free (node);
    }
  while ((node = device->used_fp_list) != NULL)
    {
      device->used_fp_list = node->fp_next;
      free (node);
    }
  free (device->fp_bitmap);
  free (device->storage);
}

static void
destroy_mm (struct mm_struct *mm)
{
  struct vm_area_struct *area;

  pt_destroy_tree (mm);
  while ((area = mm->mmap) != NULL)
    {
      mm->mmap = area->vm_next;
      free (area);
    }
  free (mm);
}

static void
check_roundtrip (int swap_id)
{
  struct memphy_struct ram = {0};
  struct memphy_struct swaps[PAGING_MAX_MMSWP] = {0};
  struct mmpaging_ld_args args = {0};
  struct pcb_t process = {0};
  struct pt_translation translation;
  addr_t addresses[3];
  BYTE value;
  int region, offset, id;

  memset (&os, 0, sizeof (os));
  memramsz = 2 * PAGING64_PAGESZ;
  memset (memswpsz, 0, sizeof (memswpsz));
  memswpsz[swap_id] = 16 * PAGING64_PAGESZ;
  assert (init_memory_devices (&args, &ram, swaps) == 0);
  assert (args.active_mswp == &swaps[swap_id]);
  assert (args.active_mswp_id == swap_id);
  for (id = 0; id < PAGING_MAX_MMSWP; id++)
    assert (args.mswp[id] == (id == swap_id ? &swaps[id] : NULL));

  os.mram = args.mram;
  os.mswp = args.mswp;
  os.active_mswp = args.active_mswp;
  os.active_mswp_id = args.active_mswp_id;
  os.mm = calloc (1, sizeof (*os.mm));
  assert (os.mm != NULL && init_mm (os.mm, NULL) == 0);
  process.pid = 1;
  process.krnl = &os;
  process.mm = calloc (1, sizeof (*process.mm));
  assert (process.mm != NULL && init_mm (process.mm, &process) == 0);
  assert (registry_register (&os, &process) == 0);

  for (region = 0; region < 3; region++)
    {
      assert (__alloc (&process, 0, region, PAGING64_PAGESZ,
                       &addresses[region]) == 0);
      for (offset = 0; offset < PAGING64_PAGESZ; offset++)
        assert (__write (&process, 0, region, offset,
                         (BYTE)((offset + region * 37) % 127)) == 0);
    }
  assert (pt_translate_va (&process, addresses[0], PT_USER_READ,
                           &translation) == PT_STATUS_SWAPPED);
  assert (translation.swptyp == swap_id);

  /* A missing device must not silently redirect this slot to active_mswp. */
  args.mswp[swap_id] = NULL;
  value = 99;
  assert (__read (&process, 0, 0, 0, &value) == -1);
  assert (value == 99);
  assert (ram.nfree == 0);
  assert (swaps[swap_id].nfree == swaps[swap_id].numfp - 1);
  args.mswp[swap_id] = &swaps[swap_id];

  /* Cycle all three pages through two RAM frames, verifying every byte. */
  for (int pass = 0; pass < 3; pass++)
    for (region = 0; region < 3; region++)
      {
        for (offset = 0; offset < PAGING64_PAGESZ; offset++)
          {
            assert (__read (&process, 0, region, offset, &value) == 0);
            assert (value == (BYTE)((offset + region * 37) % 127));
          }
        assert (MEMPHY_verify (&ram) == 0);
        assert (MEMPHY_verify (&swaps[swap_id]) == 0);
      }

  kernel_cleanup_process (&process);
  assert (process.mm == NULL);
  assert (ram.nfree == ram.numfp);
  assert (swaps[swap_id].nfree == swaps[swap_id].numfp);
  assert (MEMPHY_verify (&ram) == 0);
  assert (MEMPHY_verify (&swaps[swap_id]) == 0);
  destroy_mm (os.mm);
  destroy_device (&ram);
  for (id = 0; id < PAGING_MAX_MMSWP; id++)
    destroy_device (&swaps[id]);
}

static void
check_disabled_swaps (void)
{
  struct memphy_struct ram = {0};
  struct memphy_struct swaps[PAGING_MAX_MMSWP] = {0};
  struct mmpaging_ld_args args = {0};
  int id;

  memramsz = 2 * PAGING64_PAGESZ;
  memset (memswpsz, 0, sizeof (memswpsz));
  assert (init_memory_devices (&args, &ram, swaps) == -1);
  assert (args.active_mswp == NULL && args.active_mswp_id == -1);
  destroy_device (&ram);
  for (id = 0; id < PAGING_MAX_MMSWP; id++)
    {
      assert (args.mswp[id] == NULL);
      destroy_device (&swaps[id]);
    }
}

static int
count_process (struct pcb_t *p, void *ctx)
{
  assert (p != NULL && p != REG_TOMBSTONE);
  (*(int *)ctx)++;
  return 0;
}

static void
check_merge_regressions (void)
{
  struct krnl_t kernel = {0};
  struct pcb_t a = {.pid = 1}, b = {.pid = 1 + MAX_PROC};
  struct pcb_t c = {.pid = 1 + 2 * MAX_PROC};
  int count = 0, cursor = 0;
  assert (registry_register (&kernel, &a) == 0);
  assert (registry_register (&kernel, &b) == 0);
  assert (registry_unregister (&kernel, a.pid) == 0);
  assert (registry_lookup (&kernel, b.pid) == &b);
  assert (registry_foreach_process (&kernel, count_process, &count) == 1);
  assert (count == 1);
  count = 0;
  assert (registry_for_each_locked (&kernel, count_process, &count) == 0);
  assert (count == 1);
  assert (registry_iter (&kernel, &cursor) == &b);
  assert (registry_iter (&kernel, &cursor) == NULL);
  assert (registry_register (&kernel, &b) == -1);
  assert (registry_register (&kernel, &c) == 0);
  assert (registry_lookup (&kernel, c.pid) == &c);

  struct mm_struct mm = {0};
  struct pcb_t process = {.mm = &mm};
  addr_t va = 0;
  mm.symrgtbl[0].active = 1;
  mm.symrgtbl[0].vmaid = 0;
  mm.symrgtbl[0].rg_start = 100;
  mm.symrgtbl[0].rg_end = 110;
  assert (vm_validate_active_region (&process, 0, 9) == 0);
  assert (vm_validate_active_region (&process, 0, 10) == -1);
  assert (vm_validate_active_region (&process, -1, 0) == -1);
  assert (vm_validate_active_access (&process, 0, 0, 8, 2, &va) == 0);
  assert (va == 108);
  assert (vm_validate_active_access (&process, 0, 0, 8, 3, &va) == -1);
  assert (vm_validate_active_access (&process, 1, 0, 0, 1, &va) == -1);
  assert (vm_validate_active_access (&process, 0, 0, 0, (addr_t)-1, &va) == -1);
  mm.symrgtbl[0].active = 0;
  assert (vm_validate_active_region (&process, 0, 0) == -1);
  mm.symrgtbl[0].active = 1;
  mm.symrgtbl[0].rg_start = kernel_base;
  mm.symrgtbl[0].rg_end = kernel_base + 10;
  assert (vm_validate_active_access (&process, 0, 0, 0, 1, &va) == -1);

  addr_t first, second, reused;
  assert (kva_alloc (&kernel, PAGING64_PAGESZ, &first) == 0);
  assert (first == kernel_base);
  assert (kva_alloc (&kernel, PAGING64_PAGESZ, &second) == 0);
  assert (kva_free (&kernel, first, PAGING64_PAGESZ) == 0);
  assert (kva_alloc (&kernel, PAGING64_PAGESZ, &reused) == 0);
  assert (reused == first);
  assert (kva_free (&kernel, second, PAGING64_PAGESZ) == 0);
  assert (kva_free (&kernel, first, PAGING64_PAGESZ) == 0);
  assert (kernel.kva_free == NULL && kernel.kva_brk == kernel_base);
  puts ("Merge regression: PASS (registry collisions/tombstones, access bounds, KVA reuse)");
}

int
main (void)
{
  check_merge_regressions ();
  check_roundtrip (0);
  check_roundtrip (1);
  check_roundtrip (3);
  check_disabled_swaps ();
  puts ("SWAP round-trip and cleanup: PASS (devices 0, 1, 3)");
  return 0;
}
