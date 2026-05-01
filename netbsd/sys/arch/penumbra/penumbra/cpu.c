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

static void
cpu_attach(device_t parent, device_t self, void *aux)
{
	char name[17];
	char feat[64];
	uint32_t isa, freq;

	read_cpu_name(name);

	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(isa)  : "i"(SYSDEV_CPU),  "i"(CPU_ISA));
	__asm __volatile("RDSYS %0, %1, %2"
	    : "=r"(freq) : "i"(SYSDEV_MACH), "i"(MACH_CPU_FREQ));

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

	cpu_info_store.ci_dev = self;
	cpu_info_store.ci_cpuid = 0;
}
