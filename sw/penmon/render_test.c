/*	$NetBSD$	*/
/*
 * Host unit test for the renderer's fixed-point readouts (render.c).
 *
 *   make test            (from sw/penmon)
 *
 * Every value penmon displays is a scaled integer split into an "N.M"
 * readout at print time, so the split — not the sampling — is where a
 * wrong scale shows up.  Each case below sets a rate to a value whose
 * correct rendering is unambiguous and asserts the text of the row it
 * lands on, via scr_row_text() so the assertions survive a layout tweak.
 *
 * A zeroed frame is the other half of the test: it is what the first
 * refresh after start-up actually passes in, and it puts a 0 in every
 * divisor the renderer derives a gauge from.
 */
#include "penmon.h"
#include "screen.h"

#include <stdio.h>
#include <string.h>

#define TEST_ROWS 24
#define TEST_COLS 100

static int failures;

static void
check(int cond, const char *msg)
{
	printf("%s %s\n", cond ? "ok  " : "FAIL", msg);
	if (!cond)
		failures++;
}

/* Assert that row `y` of the drawn frame contains `want`. */
static void
check_row(int y, const char *want, const char *msg)
{
	char row[TEST_COLS + 1];

	scr_row_text(y, row, sizeof(row));
	if (strstr(row, want) == NULL) {
		printf("FAIL %s\n     want \"%s\"\n     row  \"%s\"\n",
		    msg, want, row);
		failures++;
		return;
	}
	printf("ok   %s\n", msg);
}

/* Rows the fixed-layout dashboard puts each panel on. */
enum { ROW_TITLE = 0, ROW_CPU = 2, ROW_CPI = 3, ROW_STALL = 4,
       ROW_L1I = 8, ROW_MEM = 12, ROW_ACT = 13, ROW_PROC0 = 16 };

int
main(void)
{
	struct rates r;
	struct history h;
	struct meminfo mem;
	struct procinfo proc;

	if (scr_init_mem(TEST_ROWS, TEST_COLS) < 0) {
		fprintf(stderr, "render_test: cannot allocate screen\n");
		return 1;
	}
	history_init(&h);

	/*
	 * A start-up frame: no interval has elapsed, so every counter
	 * delta is 0 and so is every derived rate.  The gauges must render
	 * rather than divide by their own zero scale.
	 */
	memset(&r, 0, sizeof(r));
	memset(&mem, 0, sizeof(mem));
	render_frame(&r, &h, &mem, 0, 0, NULL, 0, 1000, "Penumbra/2");
	check(1, "zeroed frame renders without a divide-by-zero");
	check_row(ROW_CPI, " 0.00", "zeroed CPI reads 0.00");
	check_row(ROW_L1I, "  0.0%", "zeroed hit rate reads 0.0%");

	/* A populated frame: one distinctive value per readout. */
	memset(&r, 0, sizeof(r));
	r.dt_us = 1000000;
	r.clk_khz = 62500;			/* 62.5 MHz */
	r.cpi = 372;				/* 3.72 cycles/insn */
	r.mips = 1680;				/* 16.80 MIPS */
	r.cpu_pct[CP_USER] = 123;		/* 12.3% -> rounds to 12 */
	r.cpu_pct[CP_SYS] = 45;			/* 4.5%  -> rounds to 5  */
	r.cpu_pct[CP_IDLE] = 832;		/* 83.2% -> rounds to 83 */
	r.stall_ifetch_pct = 405;		/* 40.5% */
	r.l1i.hit_pct = 998;			/* 99.8% */
	r.l1i.miss_per_sec = 1234;
	r.faults_per_sec = 77;
	r.intr_per_sec = 250;
	r.syscall_per_sec = 4096;
	mem.total_bytes = 32ULL * 1024 * 1024;
	mem.free_bytes = 8ULL * 1024 * 1024;

	history_push(&h, &r);

	proc.pid = 421;
	proc.pctcpu = 456;			/* 45.6% */
	proc.rss_bytes = 1536 * 1024;		/* 1.5M */
	proc.state = 'R';
	strcpy(proc.user, "root");
	strcpy(proc.comm, "penmon");

	/* load 1.25, up 01:01:01, refresh every 2.0 s */
	render_frame(&r, &h, &mem, 125, 3661, &proc, 1, 2000, "Penumbra/2");

	check_row(ROW_TITLE, "Penumbra/2 @ 62.5 MHz", "clock reads 62.5 MHz");
	check_row(ROW_TITLE, "up 01:01:01", "uptime splits into hh:mm:ss");
	check_row(ROW_TITLE, "load avg 1.25", "load average keeps two decimals");
	check_row(ROW_CPU, "us  12% sy   5%", "CPU percentages round to whole");
	check_row(ROW_CPU, "id  83%", "idle percentage rounds to whole");
	check_row(ROW_CPI, " 3.72", "CPI keeps two decimals");
	check_row(ROW_CPI, "MIPS  16.80", "MIPS keeps two decimals");
	check_row(ROW_STALL, "ifetch 40.5%", "stall percentage keeps one decimal");
	check_row(ROW_L1I, " 99.8%", "hit rate keeps one decimal");
	check_row(ROW_L1I, "   1234/s", "miss rate is a whole count");
	check_row(ROW_MEM, "24.0M / 32.0M used", "byte counts scale to MiB");
	check_row(ROW_MEM, "(8.0M free)", "free memory scales to MiB");
	check_row(ROW_MEM, "flt 77/s", "fault rate is a whole count");
	check_row(ROW_ACT, "intr   250/s  syscall   4096/s",
	    "activity rates are whole counts");
	check_row(ROW_PROC0, "   421 root", "process row carries pid and user");
	check_row(ROW_PROC0, " 45.6", "process %CPU keeps one decimal");
	check_row(ROW_PROC0, "1.5M R", "process RSS scales to MiB");
	check_row(TEST_ROWS - 1, "interval (2.0s)", "interval reads in seconds");

	scr_shutdown();
	printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED",
	    failures);
	return failures != 0;
}
