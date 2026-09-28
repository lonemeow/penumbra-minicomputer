/*
 * Test for the main-TLB invalidation routines in
 * netbsd/sys/arch/penumbra/penumbra/tlb_subr.S.
 *
 * The runner assembles tlb_subr.S as the kernel build does and runs this
 * program on the ISS in hosted mode: supervisor, MMU off.  WRSYS and RDSYS
 * reach the TLB arrays without translating anything, so each case clears
 * every slot, seeds the ones it needs, calls the real routine and reads the
 * slots back.  The checks are the contracts pmap.c relies on.
 */

#include <machine/sysreg.h>
#include <machine/pmap.h>

typedef unsigned int u32;

void tlb_invalidate_all(void);
void tlb_invalidate_asid(int asid);
void tlb_invalidate_addr(unsigned long va, int asid);
void hosted_write(int fd, const char *buf, int len);

/* TLB_INDEX of a main-TLB slot: the way sits above the set bits. */
#define MAIN_SLOT(set, way)	((way) * TLB_NSETS + (set))

#define USER_FLAGS	(PTE_V | PTE_R | PTE_W | PTE_U)

/* Two user pages that share set 5, a user page in set 6, and a kernel
 * page in set 5. */
#define VA_A		0x00105000u
#define VA_B		0x00125000u
#define VA_OTHER	0x00106000u
#define KVA		0x80345000u
#define SET_AB		5

static int failures;

static void put(const char *s)
{
	int n = 0;
	while (s[n])
		n++;
	hosted_write(1, s, n);
}

static void check(int ok, const char *what)
{
	if (!ok) {
		put("FAIL: ");
		put(what);
		put("\n");
		failures++;
	}
}

static void select_slot(u32 index)
{
	__asm__ volatile("WRSYS %0, %1, %2" : : "r"(index),
	    "n"(SYSDEV_MMU), "n"(MMU_TLB_INDEX) : "memory");
}

static void write_slot(u32 index, u32 vpn_word, u32 pte)
{
	select_slot(index);
	__asm__ volatile("WRSYS %0, %1, %2" : : "r"(vpn_word),
	    "n"(SYSDEV_MMU), "n"(MMU_TLB_VPN) : "memory");
	__asm__ volatile("WRSYS %0, %1, %2" : : "r"(pte),
	    "n"(SYSDEV_MMU), "n"(MMU_TLB_PTE) : "memory");
}

static int slot_valid(u32 index)
{
	u32 pte;

	select_slot(index);
	__asm__ volatile("RDSYS %0, %1, %2" : "=r"(pte) :
	    "n"(SYSDEV_MMU), "n"(MMU_TLB_PTE) : "memory");
	return pte & PTE_V;
}

static u32 vpn_word(u32 va, u32 asid)
{
	return (va >> 12) << TLB_VPN_SHIFT | asid;
}

/* Map va to itself in the given way of its set, as the miss handler
 * would: global entries under ASID 0. */
static void install(u32 way, u32 va, u32 asid, u32 flags)
{
	write_slot(MAIN_SLOT((va >> 12) % TLB_NSETS, way), vpn_word(va, asid),
	    (va & PTE_PPN_MASK) | flags);
}

static void clear_all(void)
{
	for (u32 i = 0; i < TLB_NSLOTS; i++)
		write_slot(i, 0, 0);
	for (u32 i = 0; i < PTLB_NSLOTS; i++)
		write_slot(TLB_INDEX_PINNED | i, 0, 0);
}

static void test_addr(void)
{
	clear_all();
	install(0, VA_A, 3, USER_FLAGS);
	install(1, VA_B, 3, USER_FLAGS);
	tlb_invalidate_addr(VA_A, 3);
	check(!slot_valid(MAIN_SLOT(SET_AB, 0)), "addr: a way-0 match is dropped");
	check(slot_valid(MAIN_SLOT(SET_AB, 1)), "addr: its way-1 neighbour stays");

	clear_all();
	install(0, VA_A, 3, USER_FLAGS);
	install(1, VA_B, 3, USER_FLAGS);
	tlb_invalidate_addr(VA_B, 3);
	check(slot_valid(MAIN_SLOT(SET_AB, 0)), "addr: its way-0 neighbour stays");
	check(!slot_valid(MAIN_SLOT(SET_AB, 1)), "addr: a way-1 match is dropped");

	/* The same page under another ASID, and a page in another set,
	 * match nothing. */
	clear_all();
	install(0, VA_A, 3, USER_FLAGS);
	install(1, VA_B, 3, USER_FLAGS);
	tlb_invalidate_addr(VA_A, 4);
	tlb_invalidate_addr(VA_OTHER, 3);
	check(slot_valid(MAIN_SLOT(SET_AB, 0)) && slot_valid(MAIN_SLOT(SET_AB, 1)),
	    "addr: another ASID or set matches nothing");

	clear_all();
	install(1, KVA, 0, PTE_KERNEL);
	tlb_invalidate_addr(KVA, 0);
	check(!slot_valid(MAIN_SLOT(SET_AB, 1)), "addr: a global kernel entry is dropped by ASID 0");
}

static void test_asid(void)
{
	clear_all();
	install(0, VA_A, 3, USER_FLAGS);
	install(1, VA_A, 4, USER_FLAGS);
	install(0, VA_OTHER, 3, USER_FLAGS | PTE_G);
	install(1, VA_OTHER, 3, USER_FLAGS);
	tlb_invalidate_asid(3);
	check(!slot_valid(MAIN_SLOT(SET_AB, 0)) && !slot_valid(MAIN_SLOT(6, 1)),
	    "asid: the ASID's entries are dropped");
	check(slot_valid(MAIN_SLOT(SET_AB, 1)), "asid: another ASID's entry stays");
	check(slot_valid(MAIN_SLOT(6, 0)), "asid: a global entry stays whatever its ASID field");
}

static void test_all(void)
{
	int left = 0;

	clear_all();
	for (u32 i = 0; i < TLB_NSLOTS; i++)
		write_slot(i, vpn_word((0x100 + i) << 12, 3), ((0x100 + i) << 12) | USER_FLAGS);
	write_slot(PTLB_SCRATCH, vpn_word(SCRATCH_VA, 0), 0x2000 | PTE_KERNEL_NC);
	tlb_invalidate_all();
	for (u32 i = 0; i < TLB_NSLOTS; i++)
		left += slot_valid(i) != 0;
	check(left == 0, "all: every main slot is dropped");
	check(slot_valid(PTLB_SCRATCH), "all: pinned slots stay");
}

int main(void)
{
	test_addr();
	test_asid();
	test_all();
	if (failures)
		put("tlb-test: failures\n");
	else
		put("tlb-test: all passed\n");
	return failures != 0;
}
