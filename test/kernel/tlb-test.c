/*
 * Test for the main-TLB invalidation routines in
 * netbsd/sys/arch/penumbra/penumbra/tlb_subr.S.
 *
 * The runner assembles tlb_subr.S as the kernel build does and runs this
 * program on the ISS in hosted mode: supervisor, MMU off, once under each
 * MMU the kernel drives.  WRSYS and RDSYS reach the TLB arrays without
 * translating anything, so each case clears every slot, seeds the ones it
 * needs, calls the real routine and reads the slots back.  The checks are
 * the contracts pmap.c relies on.  Slots are found from MMU_CONFIG, as the
 * routines find them.
 */

#include <machine/sysreg.h>
#include <machine/pmap.h>

typedef unsigned int u32;

void tlb_invalidate_all(void);
void tlb_invalidate_asid(int asid);
void tlb_invalidate_addr(unsigned long va, int asid);
void hosted_write(int fd, const char *buf, int len);

/* The MMU's geometry, from MMU_CONFIG: main slots and the pinned select. */
static u32 n_sets, n_slots, pinned_select;

/* TLB_INDEX of a main-TLB slot: way * sets + set. */
#define MAIN_SLOT(set, way)	((way) * n_sets + (set))

#define USER_FLAGS	(PTE_V | PTE_R | PTE_W | PTE_U)

/* Two user pages that share set 5, a user page in set 6, and a kernel
 * page in set 5, under every geometry here: their VPNs differ by
 * multiples of 256 sets. */
#define VA_A		0x00105000u
#define VA_B		0x00205000u
#define VA_OTHER	0x00106000u
#define KVA		0x80305000u
#define SET_AB		5

static int failures;

static void read_geometry(void)
{
	u32 cfg;

	__asm__ volatile("RDSYS %0, %1, %2" : "=r"(cfg) :
	    "n"(SYSDEV_MMU), "n"(MMU_CONFIG));
	if (cfg == 0)
		cfg = MMUCFG_V1_8P_32X2;
	n_sets = 1u << ((cfg >> MMUCFG_SETS_LOG2_SHIFT) & 0x1F);
	n_slots = n_sets * ((cfg >> MMUCFG_WAYS_SHIFT) & 0x1F);
	pinned_select = (cfg & MMUCFG_VER_MASK) >= 2 ?
	    TLB_INDEX_PINNED_V2 : TLB_INDEX_PINNED_V1;
}

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
	write_slot(MAIN_SLOT((va >> 12) % n_sets, way), vpn_word(va, asid),
	    (va & PTE_PPN_MASK) | flags);
}

static void clear_all(void)
{
	for (u32 i = 0; i < n_slots; i++)
		write_slot(i, 0, 0);
	for (u32 i = 0; i < PTLB_NSLOTS; i++)
		write_slot(pinned_select | i, 0, 0);
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

/*
 * A dead slot's VPN word is never a match.  Invalidating a slot writes
 * only TLB_PTE, which commits the entry with whatever TLB_VPN was last
 * staged, so a dead slot keeps an arbitrary word -- and it can name the
 * live entry in the other way of its set.  The kernel gets there when
 * the last word staged belongs to a page in the set it invalidates in:
 *
 *   1. a refill installs page Z in way 1, staging Z's word;
 *   2. before anything else stages a word, the kernel invalidates page
 *      X in way 0 of the same set, which commits {Z, V=0} there;
 *   3. unmapping Z later, tlb_invalidate_addr(Z) meets the dead way-0
 *      copy before the live one.
 *
 * pmap.c seldom lines this up: looking up a PTE maps the scratch window,
 * which stages that window's word just before the invalidation.  A
 * refill between the lookup and the invalidation can, and so can
 * tlb_invalidate_all() at ASID rollover, which leaves every slot holding
 * the last word staged.  The first case builds the dead copy directly;
 * the second reaches it through steps 1-3, install() staging each word
 * as the refill does.
 */
static void test_dead_slot(void)
{
	clear_all();
	install(0, VA_A, 3, USER_FLAGS & ~PTE_V);
	install(1, VA_A, 3, USER_FLAGS);
	tlb_invalidate_addr(VA_A, 3);
	check(!slot_valid(MAIN_SLOT(SET_AB, 1)), "dead_slot: did not invalidate way-1");

	clear_all();
	install(0, VA_B, 3, USER_FLAGS);	/* X */
	install(1, VA_A, 3, USER_FLAGS);	/* Z, staged last */
	tlb_invalidate_addr(VA_B, 3);
	tlb_invalidate_addr(VA_A, 3);
	check(!slot_valid(MAIN_SLOT(SET_AB, 1)),
	    "dead_slot: an invalidation's leftover word does not hide way 1");
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
	for (u32 i = 0; i < n_slots; i++)
		write_slot(i, vpn_word((0x100 + i) << 12, 3), ((0x100 + i) << 12) | USER_FLAGS);
	write_slot(pinned_select | PTLB_SLOT_SCRATCH, vpn_word(SCRATCH_VA, 0),
	    0x2000 | PTE_KERNEL_NC);
	tlb_invalidate_all();
	for (u32 i = 0; i < n_slots; i++)
		left += slot_valid(i) != 0;
	check(left == 0, "all: every main slot is dropped");
	check(slot_valid(pinned_select | PTLB_SLOT_SCRATCH), "all: pinned slots stay");
}

int main(void)
{
	read_geometry();
	test_addr();
	test_dead_slot();
	test_asid();
	test_all();
	if (failures)
		put("tlb-test: failures\n");
	else
		put("tlb-test: all passed\n");
	return failures != 0;
}
