/*
 * ras-test — does a captured context ever name the middle of a
 * restartable atomic sequence?
 *
 * The ISA has no compare-and-swap, so libc builds one as a restartable
 * sequence: the kernel rewinds the PC to the start whenever the
 * sequence is interrupted.  Rewinding the trapframe covers a thread
 * that resumes where it left off.  A context *copied out* -- the frame
 * sendsig builds, or setcontext(2) -- has to carry the rewound PC too,
 * or resuming from it finishes a sequence whose loaded value is already
 * stale.
 *
 * The sequence here is this program's own, registered with rasctl(2),
 * rather than the one inside libc: the bounds of libc's are not
 * addressable from a non-PIC executable, and a static link leaves its
 * registration behind entirely.  It is a copy of the libc sequence, so
 * the property under test is the same one.
 *
 * A tight loop runs it under an interval timer, and the signal handler
 * reads the interrupted PC out of the context.  The sequence is four of
 * the handful of instructions the loop executes, so a kernel that does
 * not rewind is caught within a few hundred samples.
 */

#include <sys/types.h>
#include <sys/ras.h>
#include <sys/time.h>

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>

/*
 * A copy of libc's _atomic_cas_32, with its own markers: the store is
 * the last instruction and nothing inside writes an argument register,
 * so restarting at the top is indistinguishable from never having
 * begun.  The end marker sits one past the store, so a PC on the store
 * still restarts.
 */
extern char ras_seq_start[], ras_seq_end[];
uint32_t test_cas(volatile uint32_t *, uint32_t, uint32_t);

/* libc's own sequence, registered by its atomic initialization. */
extern char _atomic_cas_32_ras_start[];
extern char _atomic_cas_32_ras_end[];

__asm__(
	"	.text\n"
	"	.globl	test_cas\n"
	"	.type	test_cas, @function\n"
	"	.globl	ras_seq_start\n"
	"	.globl	ras_seq_end\n"
	"test_cas:\n"
	"ras_seq_start:\n"
	"	ldw	r11, [r1]\n"
	"	cmp	r11, r2\n"
	"	bne	1f\n"
	"	stw	r3, [r1]\n"
	"ras_seq_end:\n"
	"1:\n"
	"	mov	r1, r11\n"
	"	ret\n");

static volatile sig_atomic_t samples;
static volatile sig_atomic_t inside;
static volatile uintptr_t    first_bad;

static void
onalarm(int sig, siginfo_t *si, void *ctx)
{
	const ucontext_t *uc = ctx;
	uintptr_t pc = (uintptr_t)uc->uc_mcontext.__gregs[_REG_PC];

	samples++;

	/* Open at both ends, as ras_lookup() matches. */
	if (pc > (uintptr_t)ras_seq_start && pc < (uintptr_t)ras_seq_end) {
		if (inside++ == 0)
			first_bad = pc;
	}
}

int
main(void)
{
	struct sigaction sa;
	struct itimerval it;
	volatile uint32_t word = 0;
	unsigned long spins = 0;
	size_t len = (size_t)(ras_seq_end - ras_seq_start);

	if (rasctl(ras_seq_start, len, RAS_INSTALL) != 0) {
		perror("rasctl");
		return 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = onalarm;
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGALRM, &sa, NULL) != 0) {
		perror("sigaction");
		return 1;
	}

	/* As often as the clock allows: every sample is a chance to catch it. */
	it.it_interval.tv_sec = 0;
	it.it_interval.tv_usec = 1000;
	it.it_value = it.it_interval;
	if (setitimer(ITIMER_REAL, &it, NULL) != 0) {
		perror("setitimer");
		return 1;
	}

	printf("sequence %p..%p, %zu bytes\n",
	    (void *)ras_seq_start, (void *)ras_seq_end, len);

	while (samples < 500) {
		/* Alternate the value so the store is always reached. */
		test_cas(&word, word, word ^ 1u);
		word ^= 1u;
		spins++;
	}

	it.it_value.tv_sec = it.it_value.tv_usec = 0;
	it.it_interval = it.it_value;
	setitimer(ITIMER_REAL, &it, NULL);

	printf("%lu spins, %d samples, %d inside the sequence\n",
	    spins, (int)samples, (int)inside);
	if (inside != 0) {
		printf("FAIL: a captured context kept a PC inside the "
		    "sequence (first %p)\n", (void *)first_bad);
		return 1;
	}
	printf("PASS: every captured context named the sequence start or "
	    "a PC outside it\n");

	/*
	 * And libc's own sequence has to be registered, in this binary as
	 * in any other: installing over a registered range is refused,
	 * which is the only way to ask without disturbing it.
	 */
	len = (size_t)(_atomic_cas_32_ras_end - _atomic_cas_32_ras_start);
	if (rasctl(_atomic_cas_32_ras_start, len, RAS_INSTALL) == 0 ||
	    errno != EEXIST) {
		printf("FAIL: libc left its CAS sequence unregistered "
		    "(%p, %zu bytes)\n",
		    (void *)_atomic_cas_32_ras_start, len);
		return 1;
	}
	printf("PASS: libc registered its CAS sequence\n");
	return 0;
}
