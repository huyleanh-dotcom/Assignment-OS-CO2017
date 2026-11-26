#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
swap_test_dir=$(mktemp -d /tmp/os-swap-test.XXXXXX)
trap 'rm -f "$swap_test_dir/syscalltbl.lst" "$swap_test_dir/swap_roundtrip"; rmdir "$swap_test_dir"' EXIT
sh src/syscalltbl.sh src/syscall.tbl "$swap_test_dir/syscalltbl.lst"
${CC:-gcc} -std=gnu11 -g -ffunction-sections -fdata-sections \
  ${SWAP_TEST_CFLAGS:-} -Iinclude -I"$swap_test_dir" \
  test/swap_roundtrip.c src/mm-memphy.c src/mm64.c src/mm-vm.c \
  src/libmem.c src/syscall.c src/sys_mem.c src/sys_listsyscall.c \
  -Wl,--gc-sections -pthread -o "$swap_test_dir/swap_roundtrip"
"$swap_test_dir/swap_roundtrip"
