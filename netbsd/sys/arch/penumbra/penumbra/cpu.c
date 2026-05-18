/*	$NetBSD$	*/

/*
 * Penumbra CPU device — attaches at mainbus.
 *
 * On attach, reads CPU identity (name + ISA + features) from
 * SYSDEV_CPU and the CPU clock frequency from SYSDEV_MACH, and
 * prints a detailed `cpu0 at mainbus0` line.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/cpu.h>
#include <sys/endian.h>

#include <machine/cpu.h>
#include <machine/sysreg.h>

static int	cpu_match(device_t, cfdata_t, void *);
static void	cpu_attach(device_t, device_t, void *);

/* CPU clock frequency in Hz, read from SYSDEV_MACH at cpu_attach time.
 * Consumed by cpu_frequency() in <machine/cpu_counter.h>. */
uint32_t cpu_clock_freq_hz;

CFATTACH_DECL_NEW(cpu, 0,
    cpu_match, cpu_attach, NULL, NULL);

static int
cpu_match(device_t parent, cfdata_t cf, void *aux)
{

	return 1;
}

/*
 * Read the 16-byte CPU name string from SYSDEV_CPU regs CPU_NAME0..3.
 *
 * RDSYS encodes its register number as an instruction immediate, so
 * each register gets its own asm with a literal constant — we can't
 * index it at runtime.
 */
static void
read_cpu_name(char *buf)
{
	uint32_t w0, w1, w2, w3;

	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(w0) : "i"(SYSDEV_CPU), "i"(CPU_NAME0));
	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(w1) : "i"(SYSDEV_CPU), "i"(CPU_NAME1));
	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(w2) : "i"(SYSDEV_CPU), "i"(CPU_NAME2));
	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(w3) : "i"(SYSDEV_CPU), "i"(CPU_NAME3));

	le32enc(buf +  0, w0);
	le32enc(buf +  4, w1);
	le32enc(buf +  8, w2);
	le32enc(buf + 12, w3);
	buf[16] = '\0';
}

/*
 * Decode the CPU_ISA register into a human-readable feature string.
 *
 *   bits [3:0]  ISA version
 *   bits [31:4] optional feature flags (see CPU_FEAT_BIT_* in sysreg.h)
 *
 * Target output format (matches the boot ROM banner):
 *   "ISA v1"                 — base ISA, no optional features
 *   "ISA v1, MUL"            — single feature
 *   "ISA v1, MUL, DIV, FPU"  — multiple features
 *   "ISA v1, MUL, UNK_5"     — unknown future bit
 */
static void
format_cpu_features(char *buf, size_t bufsz, uint32_t isa)
{
	snprintf(buf, bufsz, "ISA v%u", isa & 0xFu);

	for (int bit = 0; bit < 28; bit++)
	{
		/* Feature flags start at bit 4 */
		uint32_t mask = 1u << (bit + 4);
		/* TODO: flag meanings could differ per ISA version */
		if (isa & mask)
		{
			switch (bit)
			{
			case CPU_FEAT_BIT_HW_MUL:
				strncat(buf, ", MUL", bufsz);
				break;
			case CPU_FEAT_BIT_HW_DIV:
				strncat(buf, ", DIV", bufsz);
				break;
			case CPU_FEAT_BIT_FPU:
				strncat(buf, ", FPU", bufsz);
				break;
			default:
			{
				char tmp[12];
				snprintf(tmp, sizeof(tmp), ", UNK_%d", bit);
				strncat(buf, tmp, bufsz);
				break;
			}
			}
		}
	}
}

/*
 * Decode the 2-bit CACHE_INFO_ADDRESSING field (PIPT/VIPT/VIVT) into a tag.
 * Mirrors format_cache_addressing() in hw/rom/boot_rom.c.
 */
static void
format_cache_addressing(char *buf, size_t bufsz, uint32_t addr)
{
	if (bufsz > 0)
		buf[0] = '\0';

	switch (addr) {
	case CACHE_ADDR_PIPT: strncat(buf, "PIPT", bufsz); break;
	case CACHE_ADDR_VIPT: strncat(buf, "VIPT", bufsz); break;
	case CACHE_ADDR_VIVT: strncat(buf, "VIVT", bufsz); break;
	default:
		snprintf(buf, bufsz, "UNK_%u", addr);
		break;
	}
}

/*
 * Format a byte count as "%uB" / "%ukB" / "%uMB", matching the boot ROM's
 * humanize_size().  Caches are always power-of-2 sizes so integer division
 * is sufficient.
 */
static void
humanize_size(uint32_t bytes, char *buf, size_t bufsz)
{
	if (bytes < 1024u)
		snprintf(buf, bufsz, "%uB", bytes);
	else if (bytes < 1024u * 1024u)
		snprintf(buf, bufsz, "%ukB", bytes / 1024u);
	else
		snprintf(buf, bufsz, "%uMB", bytes / (1024u * 1024u));
}

static void
print_one_cache(device_t self, const char *label, uint32_t info)
{
	uint32_t line_words = CACHE_INFO_LINE_WORDS(info);
	uint32_t num_sets   = CACHE_INFO_NUM_SETS(info);
	uint32_t num_ways   = CACHE_INFO_NUM_WAYS(info);
	uint32_t addr       = CACHE_INFO_ADDRESSING(info);
	uint32_t wb         = CACHE_INFO_WRITE_BACK(info);
	uint32_t wa         = CACHE_INFO_WRITE_ALLOC(info);
	uint32_t line_bytes  = line_words * 4u;
	uint32_t total_bytes = num_sets * num_ways * line_bytes;

	char size_str[12], addr_str[16];
	humanize_size(total_bytes, size_str, sizeof(size_str));
	format_cache_addressing(addr_str, sizeof(addr_str), addr);

	aprint_normal_dev(self, "%s: %s (%u x %uB, %u-way) %s (%s, %s)\n",
	    label,
	    size_str,
	    num_sets, line_bytes, num_ways,
	    addr_str,
	    wb ? "WB" : "WT",
	    wa ? "WA" : "WnA");
}

static void
cpu_attach(device_t parent, device_t self, void *aux)
{
	char name[17];
	char feat[64];
	uint32_t isa, freq;
	uint32_t ic_info, dc_info, l2_info;

	read_cpu_name(name);

	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(isa)  : "i"(SYSDEV_CPU),  "i"(CPU_ISA));
	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(freq) : "i"(SYSDEV_MACH), "i"(MACH_CPU_FREQ));
	cpu_clock_freq_hz = freq;

	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(ic_info) : "i"(SYSDEV_L1_ICACHE), "i"(CACHE_INFO));
	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(dc_info) : "i"(SYSDEV_L1_DCACHE), "i"(CACHE_INFO));
	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(l2_info) : "i"(SYSDEV_L2_CACHE),  "i"(CACHE_INFO));

	format_cpu_features(feat, sizeof(feat), isa);

	if (freq != 0) {
		uint32_t mhz      = freq / 1000000U;
		uint32_t khz_frac = (freq / 1000U) % 1000U;
		if (khz_frac != 0)
			aprint_normal(": %s, %s @ %u.%03u MHz\n",
			    name, feat, mhz, khz_frac);
		else
			aprint_normal(": %s, %s @ %u MHz\n",
			    name, feat, mhz);
	} else {
		aprint_normal(": %s, %s\n", name, feat);
	}

	print_one_cache(self, "L1 icache", ic_info);
	print_one_cache(self, "L1 dcache", dc_info);
	if (l2_info != 0)
		print_one_cache(self, "L2 cache ", l2_info);

	cpu_info_store.ci_dev = self;
	cpu_info_store.ci_cpuid = 0;
}
