#ifndef MM64_H
#define MM64_H

#include "mm.h"
#include <stdint.h>
#include <stddef.h>


#define MM64_BITS_PER_LONG 64

#define PAGING64_CPU_BUS_WIDTH 57 /* 57 bit bus - MAX SPACE 4MB */
#define PAGING64_PAGESZ  4096      /* 4KB or 12-bits PAGE NUMBER */

#define GENMASK64(h, l) \
	(((~0ULL) << (l)) & (~0ULL >> (MM64_BITS_PER_LONG  - (h) - 1)))

#define PAGING64_MAX_PGN  (DIV_ROUND_UP(BIT_ULL(21),PAGING64_PAGESZ))
#define PAGING64_PAGE_ALIGNSZ(sz) (DIV_ROUND_UP(sz,PAGING64_PAGESZ)*PAGING64_PAGESZ)


/* OFFSET */
#define PAGING64_ADDR_OFFST_HIBIT 11
#define PAGING64_ADDR_OFFST_LOBIT 0

/* PT */
#define PAGING64_ADDR_PT_HIBIT 20
#define PAGING64_ADDR_PT_LOBIT 12
#define PAGING64_ADDR_PT_SHIFT 12

/* PMD */
#define PAGING64_ADDR_PMD_HIBIT 29
#define PAGING64_ADDR_PMD_LOBIT 21

/* PUD */
#define PAGING64_ADDR_PUD_HIBIT 38
#define PAGING64_ADDR_PUD_LOBIT 30

/* P4D */
#define PAGING64_ADDR_P4D_HIBIT 47
#define PAGING64_ADDR_P4D_LOBIT 39

/* PGD */
#define PAGING64_ADDR_PGD_HIBIT 56
#define PAGING64_ADDR_PGD_LOBIT 48

/* Extract PGD Entry */
#define PAGING64_ADDR_OFFST(addr) GETVAL(addr,PAGING64_ADDR_OFFST_MASK,PAGING_ADDR_OFFST_LOBIT)
#define PAGING64_ADDR_PT(addr)   ((addr&PAGING64_ADDR_PT_MASK)>>PAGING64_ADDR_PT_LOBIT)
//GETVAL(addr,PAGING64_ADDR_PT_MASK,PAGING64_ADDR_PT_LOBIT)
#define PAGING64_ADDR_PMD(addr)   ((addr&PAGING64_ADDR_PMD_MASK)>>PAGING64_ADDR_PMD_LOBIT)
//GETVAL(addr,PAGING64_ADDR_PMD_MASK,PAGING64_ADDR_PMD_LOBIT)
#define PAGING64_ADDR_PUD(addr)   ((addr&PAGING64_ADDR_PUD_MASK)>>PAGING64_ADDR_PUD_LOBIT)
//GETVAL(addr,PAGING64_ADDR_PUD_MASK,PAGING64_ADDR_PUD_LOBIT)
#define PAGING64_ADDR_P4D(addr)   ((addr&PAGING64_ADDR_P4D_MASK)>>PAGING64_ADDR_P4D_LOBIT)
//GETVAL(addr,PAGING64_ADDR_P4D_MASK,PAGING64_ADDR_P4D_LOBIT)
#define PAGING64_ADDR_PGD(addr)   ((addr&PAGING64_ADDR_PGD_MASK)>>PAGING64_ADDR_PGD_LOBIT)
//GETVAL(addr,PAGING64_ADDR_PGD_MASK,PAGING64_ADDR_PGD_LOBIT)


/* Masks */
#define PAGING64_ADDR_OFFST_MASK  GENMASK64(PAGING_ADDR_OFFST_HIBIT,PAGING_ADDR_OFFST_LOBIT)
#define PAGING64_ADDR_PT_MASK  GENMASK64(PAGING64_ADDR_PT_HIBIT,PAGING64_ADDR_PT_LOBIT)
#define PAGING64_ADDR_PMD_MASK  GENMASK64(PAGING64_ADDR_PMD_HIBIT,PAGING64_ADDR_PMD_LOBIT)
#define PAGING64_ADDR_PUD_MASK  GENMASK64(PAGING64_ADDR_PUD_HIBIT,PAGING64_ADDR_PUD_LOBIT)
#define PAGING64_ADDR_P4D_MASK  GENMASK64(PAGING64_ADDR_P4D_HIBIT,PAGING64_ADDR_P4D_LOBIT)
#define PAGING64_ADDR_PGD_MASK  GENMASK64(PAGING64_ADDR_PGD_HIBIT,PAGING64_ADDR_PGD_LOBIT)

#define PAGING64_ADDR_OFFST(addr) ((((uint64_t)(addr)) & PAGING64_ADDR_OFFST_MASK) >> PAGING64_ADDR_OFFST_LOBIT)
#define PAGING64_ADDR_PT(addr)    ((((uint64_t)(addr)) & PAGING64_ADDR_PT_MASK)    >> PAGING64_ADDR_PT_LOBIT)
#define PAGING64_ADDR_PMD(addr)   ((((uint64_t)(addr)) & PAGING64_ADDR_PMD_MASK)   >> PAGING64_ADDR_PMD_LOBIT)
#define PAGING64_ADDR_PUD(addr)   ((((uint64_t)(addr)) & PAGING64_ADDR_PUD_MASK)   >> PAGING64_ADDR_PUD_LOBIT)
#define PAGING64_ADDR_P4D(addr)   ((((uint64_t)(addr)) & PAGING64_ADDR_P4D_MASK)   >> PAGING64_ADDR_P4D_LOBIT)
#define PAGING64_ADDR_PGD(addr)   ((((uint64_t)(addr)) & PAGING64_ADDR_PGD_MASK)   >> PAGING64_ADDR_PGD_LOBIT)


//* MM64 CORE : page, index, VA

#define MM64_PAGE_SHIFT      12U
#define MM64_PAGE_SIZE       (UINT64_C(1) << MM64_PAGE_SHIFT)      /* 4096 */
#define MM64_PAGE_MASK       (MM64_PAGE_SIZE - 1)                  /* 0xFFF */

#define MM64_PT_LEVELS       5U
#define MM64_PT_INDEX_BITS   9U
#define MM64_PT_ENTRIES      (1U << MM64_PT_INDEX_BITS)            /* 512 */
#define MM64_PT_INDEX_MASK   UINT64_C(0x1FF)

#define MM64_VA_BITS         57U
#define MM64_VA_SIGN_BIT     (MM64_VA_BITS - 1)                    /* 56 */
#define MM64_PGN_BITS        (MM64_VA_BITS - MM64_PAGE_SHIFT)      /* 45 */
#define MM64_PGN_MAX         ((UINT64_C(1) << MM64_PGN_BITS) - 1)

/* Các cấp, đi từ root xuống lá */
enum mm64_level {
	MM64_LVL_PGD = 0,
	MM64_LVL_P4D = 1,
	MM64_LVL_PUD = 2,
	MM64_LVL_PMD = 3,
	MM64_LVL_PT = 4
};

#define MM64_PGD_SHIFT       48U
#define MM64_P4D_SHIFT       39U
#define MM64_PUD_SHIFT       30U
#define MM64_PMD_SHIFT       21U
#define MM64_PT_SHIFT        12U

/* shift của cấp lvl: 48, 39, 30, 21, 12 */
#define MM64_LEVEL_SHIFT(lvl) (MM64_PGD_SHIFT - MM64_PT_INDEX_BITS * (unsigned)(lvl))

#define MM64_VA_INDEX(va, lvl) \
	((((uint64_t)(va)) >> MM64_LEVEL_SHIFT(lvl)) & MM64_PT_INDEX_MASK)
#define MM64_VA_OFFSET(va)       (((uint64_t)(va)) & MM64_PAGE_MASK)
#define MM64_VA_PAGE_ALIGNED(va) (MM64_VA_OFFSET(va) == 0)
#define MM64_VA_PAGE_BASE(va)    (((uint64_t)(va)) & ~MM64_PAGE_MASK)

// * LEAF PTE 64 - bit

define MM64_PTE_PRESENT(UINT64_C(1) << 0)
#define MM64_PTE_RW          (UINT64_C(1) << 1)
#define MM64_PTE_USER        (UINT64_C(1) << 2)
#define MM64_PTE_ACCESSED    (UINT64_C(1) << 5)
#define MM64_PTE_DIRTY       (UINT64_C(1) << 6)
#define MM64_PTE_SWAPPED     (UINT64_C(1) << 9)
#define MM64_PTE_NX          (UINT64_C(1) << 63)

/* Payload bits 51..12 (40 bit) */
#define MM64_PTE_PAYLOAD_SHIFT  12U
#define MM64_PTE_PAYLOAD_MASK   (((UINT64_C(1) << 40) - 1) << 12)

/* PRESENT: FPN 40-bit, cho phép FPN = 0 */
#define MM64_PTE_FPN_SHIFT      12U
#define MM64_PTE_FPN_MASK       MM64_PTE_PAYLOAD_MASK
#define MM64_PTE_FPN_MAX        ((UINT64_C(1) << 40) - 1)

/* SWAPPED: TYPE bits 16..12, OFFSET bits 48..17, bits 51..49 dự trữ */
#define MM64_PTE_SWAP_TYPE_SHIFT    12U
#define MM64_PTE_SWAP_TYPE_MASK     (UINT64_C(0x1F) << 12)
#define MM64_PTE_SWAP_TYPE_MAX      UINT64_C(0x1F)

#define MM64_PTE_SWAP_OFFSET_SHIFT  17U
#define MM64_PTE_SWAP_OFFSET_MASK   (UINT64_C(0xFFFFFFFF) << 17)
#define MM64_PTE_SWAP_OFFSET_MAX    UINT64_C(0xFFFFFFFF)

#define MM64_PTE_SWAP_RSVD_MASK     (UINT64_C(0x7) << 49)

/* Nhóm flag */
#define MM64_PTE_STATE_MASK  (MM64_PTE_PRESENT | MM64_PTE_SWAPPED)
#define MM64_PTE_PERM_MASK   (MM64_PTE_RW | MM64_PTE_USER | MM64_PTE_NX)
#define MM64_PTE_AD_MASK     (MM64_PTE_ACCESSED | MM64_PTE_DIRTY)
#define MM64_PTE_KNOWN_FLAGS (MM64_PTE_STATE_MASK | MM64_PTE_PERM_MASK | MM64_PTE_AD_MASK)

/* Bit 3-4, 7-8, 10-11, 52-62: bằng 0 */
#define MM64_PTE_RSVD_MASK   (~(MM64_PTE_KNOWN_FLAGS | MM64_PTE_PAYLOAD_MASK))

/* Flags mặc định khi tạo data mapping*/
#define MM64_PTE_DEFAULT_USER   (MM64_PTE_PRESENT | MM64_PTE_RW | MM64_PTE_USER | MM64_PTE_NX)
#define MM64_PTE_DEFAULT_KERNEL (MM64_PTE_PRESENT | MM64_PTE_RW | MM64_PTE_NX)

/* =====================================================================
 * - INTERMEDIATE ENTRY (PGD/P4D/PUD/PMD)
 *   63..12 : host pointer tới bảng con (căn 4096)
 *   11..10 : dự trữ
 *    9..0  : used_count (0..512)
 * ===================================================================== */

#define MM64_PDE_ADDR_MASK   (~UINT64_C(0xFFF))
#define MM64_PDE_RSVD_MASK   (UINT64_C(0x3) << 10)
#define MM64_PDE_COUNT_MASK  UINT64_C(0x3FF)
#define MM64_PDE_COUNT_MAX   MM64_PT_ENTRIES

 
//— KIỂU DỮ LIỆU

typedef uint64_t mm64_pte_t;   /* Leaf PTE         */
typedef uint64_t mm64_pde_t;   /* Intermediate PDE */

typedef struct mm64_table {
	uint64_t entries[MM64_PT_ENTRIES];
} mm64_table_t;

_Static_assert(sizeof(mm64_table_t) == MM64_PAGE_SIZE,
	"mm64_table_t phai dung 4096 B");
_Static_assert(sizeof(void*) <= sizeof(uint64_t),
	"host pointer phai vua 64 bit");
_Static_assert((MM64_PTE_SWAP_TYPE_MASK& MM64_PTE_SWAP_OFFSET_MASK) == 0,
	"SWAP_TYPE va SWAP_OFFSET khong duoc chong bit");
_Static_assert(((MM64_PTE_SWAP_TYPE_MASK | MM64_PTE_SWAP_OFFSET_MASK |
	MM64_PTE_SWAP_RSVD_MASK) == MM64_PTE_PAYLOAD_MASK),
	"SWAP payload phai phu kin bits 51..12");
_Static_assert((MM64_PTE_KNOWN_FLAGS& MM64_PTE_PAYLOAD_MASK) == 0,
	"flag khong duoc nam trong payload");


/* ==================================================================== =
* - ENCODE / DECODE / VALIDATE LEAF PTE(việc 1.2)
* Encoder trả 0 nếu đúng, -1 nếu sai.
* ==================================================================== = */

/* Flag được phép truyền vào encoder */
#define MM64_PTE_PRESENT_INFLAGS (MM64_PTE_PERM_MASK | MM64_PTE_AD_MASK)
#define MM64_PTE_SWAPPED_INFLAGS (MM64_PTE_PERM_MASK)

/*
 * pte_flags_validate - kiểm tra tập flag truyền vào encoder
 * @flags   : flag người gọi muốn đặt
 * @allowed : MM64_PTE_PRESENT_INFLAGS hoặc MM64_PTE_SWAPPED_INFLAGS
 * Trả 0 nếu flags chỉ chứa bit thuộc @allowed, ngược lại -1.
 * (PRESENT/SWAPPED/payload do encoder tự đặt, người gọi không được truyền.)
 */
	static inline int pte_flags_validate(uint64_t flags, uint64_t allowed)
{
	return (flags & ~allowed) ? -1 : 0;
}

/*
 * pte_encode_present - tạo Leaf PTE PRESENT
 * @fpn   : 0 .. MM64_PTE_FPN_MAX (FPN 0 hợp lệ)
 * @flags : tập con của RW | USER | NX | ACCESSED | DIRTY
 */
static inline int pte_encode_present(addr_t fpn, uint64_t flags, mm64_pte_t* out)
{
	if (out == NULL)
		return -1;
	if ((uint64_t)fpn > MM64_PTE_FPN_MAX)
		return -1;
	if (pte_flags_validate(flags, MM64_PTE_PRESENT_INFLAGS) != 0)
		return -1;

	*out = MM64_PTE_PRESENT
		| (((uint64_t)fpn << MM64_PTE_FPN_SHIFT) & MM64_PTE_FPN_MASK)
		| flags;
	return 0;
}

/*
 * pte_encode_swapped - tạo Leaf PTE SWAPPED
 * @swptyp : 0 .. 31   (giới hạn số SWAP device thực tế do P3 kiểm tra)
 * @swpoff : 0 .. 0xFFFFFFFF
 * @flags  : tập con của RW | USER | NX (giữ quyền để swap-in khôi phục);
 *           ACCESSED/DIRTY không được lưu ở trạng thái SWAPPED.
 */
static inline int pte_encode_swapped(int swptyp, addr_t swpoff, uint64_t flags,
	mm64_pte_t* out)
{
	if (out == NULL)
		return -1;
	if (swptyp < 0 || (uint64_t)swptyp > MM64_PTE_SWAP_TYPE_MAX)
		return -1;
	if ((uint64_t)swpoff > MM64_PTE_SWAP_OFFSET_MAX)
		return -1;
	if (pte_flags_validate(flags, MM64_PTE_SWAPPED_INFLAGS) != 0)
		return -1;

	*out = MM64_PTE_SWAPPED
		| (((uint64_t)swptyp << MM64_PTE_SWAP_TYPE_SHIFT) & MM64_PTE_SWAP_TYPE_MASK)
		| (((uint64_t)swpoff << MM64_PTE_SWAP_OFFSET_SHIFT) & MM64_PTE_SWAP_OFFSET_MASK)
		| flags;
	return 0;
}

/*
 * pte_decode_state - phân loại một Leaf PTE
 *   0                                   -> PT_MAP_EMPTY
 *   bit dự trữ != 0                      -> PT_MAP_INVALID
 *   PRESENT=1, SWAPPED=0                 -> PT_MAP_PRESENT
 *   PRESENT=0, SWAPPED=1, payload-rsvd=0,
 *              ACCESSED=DIRTY=0          -> PT_MAP_SWAPPED
 *   còn lại (P=S=1, P=S=0 nhưng != 0...) -> PT_MAP_INVALID
 */
static inline enum pt_mapping_state pte_decode_state(mm64_pte_t pte)
{
	uint64_t st;

	if (pte == 0)
		return PT_MAP_EMPTY;
	if (pte & MM64_PTE_RSVD_MASK)
		return PT_MAP_INVALID;

	st = pte & MM64_PTE_STATE_MASK;
	if (st == MM64_PTE_PRESENT)
		return PT_MAP_PRESENT;
	if (st == MM64_PTE_SWAPPED) {
		if (pte & (MM64_PTE_SWAP_RSVD_MASK | MM64_PTE_AD_MASK))
			return PT_MAP_INVALID;
		return PT_MAP_SWAPPED;
	}
	return PT_MAP_INVALID;
}

/* Getter thô: KHÔNG kiểm tra trạng thái — gọi pte_decode_state() trước. */
static inline addr_t pte_decode_fpn(mm64_pte_t pte)
{
	return (addr_t)((pte & MM64_PTE_FPN_MASK) >> MM64_PTE_FPN_SHIFT);
}

static inline int pte_decode_swptyp(mm64_pte_t pte)
{
	return (int)((pte & MM64_PTE_SWAP_TYPE_MASK) >> MM64_PTE_SWAP_TYPE_SHIFT);
}

static inline addr_t pte_decode_swpoff(mm64_pte_t pte)
{
	return (addr_t)((pte & MM64_PTE_SWAP_OFFSET_MASK) >> MM64_PTE_SWAP_OFFSET_SHIFT);
}

/* Quyền RW | USER | NX của PTE (giữ nguyên vị trí bit) */
static inline uint64_t pte_decode_perm(mm64_pte_t pte)
{
	return pte & MM64_PTE_PERM_MASK;
}

static inline uint64_t pte_decode_ad(mm64_pte_t pte)
{
	return pte & MM64_PTE_AD_MASK;
}
#endif
