/*
 * loops.S — memory access loops for the membench memory hierarchy tests.
 *
 * Hand-written rather than compiled, for two reasons.
 *
 * The instruction stream is frozen.  membench's numbers are recorded
 * against HEAD SHAs and compared across months, while this project's
 * compiler is itself under development; a loop the backend re-schedules
 * turns a hardware comparison into a compiler comparison.
 *
 * The access count is exact.  Each loop makes a known number of reads
 * at a known stride, so the miss count a cache counter reports can be
 * checked against what the loop must have generated.  A disagreement is
 * then a fact about the hardware — a prefetch, an aliasing effect — and
 * not a question of what the compiler emitted.
 *
 * ── load-use versus load-discard ──────────────────────────────────
 *
 * Every pattern comes in two forms, because they answer different
 * questions and a core can make them diverge.
 *
 * A load-use loop folds each result into an accumulator, so a result
 * must arrive before the loop advances.  It measures how fast the core
 * can *consume* loaded data — the number that predicts what a dependent
 * instruction stream achieves.
 *
 * A load-discard loop never reads what it loaded.  Addresses are
 * base-relative immediates in both forms, so the use-chain is the only
 * thing serialising the accesses; without it they are free to overlap
 * to whatever depth the core supports.  It measures the memory system's
 * delivery rate rather than the core's tolerance for latency.
 *
 * A core that stalls until load data returns makes the two coincide.
 * One with non-blocking loads does not, and then a single loop would
 * silently change which question it answers — so both are kept, and
 * both are named for what they do rather than for what they measure.
 *
 * Discarding is safe from the compiler's side: an external routine's
 * loads cannot be elided by a caller that cannot see them, and a load
 * is never architecturally dead in any case — it can fault, and it can
 * be a device register.  The discard loops rotate three destinations
 * so consecutive loads do not even collide on a register, which keeps
 * them independent on a core that tracks write-after-write.
 *
 * ABI: r1 = first argument and return value, r2/r3 further arguments,
 * r4 and r11 scratch.  Caller-saved is r1-r4 and r11, so a leaf loop
 * has five registers and needs no frame.  A discard loop returns zero:
 * it has nothing to report, by construction.
 */

	.equ	MEMBENCH_STRIDE, 64

	.text

/*
 * membench_seq_ldw_use(uint32_t base, uint32_t bytes) -> uint32_t
 *
 * Reads every word in [base, base + bytes) in ascending address order,
 * folding each into the returned value.  `bytes` must be a multiple of
 * 32 (the unroll).
 *
 * This is the pattern a cache line fill produces: consecutive words, so
 * a memory that answers with more than one word at a time serves the
 * rest of each line without another round trip.
 */
	.globl	membench_seq_ldw_use
	.type	membench_seq_ldw_use, @function
	.p2align 4
membench_seq_ldw_use:
	add	r2, r1			/* r2 = end address */
	mov	r3, r0			/* accumulator */
.Lseq_use:
	ldw	r4, [r1 + #0]
	xor	r3, r4
	ldw	r4, [r1 + #4]
	xor	r3, r4
	ldw	r4, [r1 + #8]
	xor	r3, r4
	ldw	r4, [r1 + #12]
	xor	r3, r4
	ldw	r4, [r1 + #16]
	xor	r3, r4
	ldw	r4, [r1 + #20]
	xor	r3, r4
	ldw	r4, [r1 + #24]
	xor	r3, r4
	ldw	r4, [r1 + #28]
	xor	r3, r4
	add	r1, #32
	cmp	r1, r2
	bne	.Lseq_use
	mov	r1, r3
	ret

/*
 * membench_seq_ldw_discard(uint32_t base, uint32_t bytes) -> 0
 *
 * The same reads as membench_seq_ldw_use, with nothing consuming them.
 */
	.globl	membench_seq_ldw_discard
	.type	membench_seq_ldw_discard, @function
	.p2align 4
membench_seq_ldw_discard:
	add	r2, r1
.Lseq_discard:
	ldw	r3,  [r1 + #0]
	ldw	r4,  [r1 + #4]
	ldw	r11, [r1 + #8]
	ldw	r3,  [r1 + #12]
	ldw	r4,  [r1 + #16]
	ldw	r11, [r1 + #20]
	ldw	r3,  [r1 + #24]
	ldw	r4,  [r1 + #28]
	add	r1, #32
	cmp	r1, r2
	bne	.Lseq_discard
	mov	r1, r0
	ret

/*
 * membench_stride_ldw_use(uint32_t base, uint32_t bytes) -> uint32_t
 *
 * Reads one word per MEMBENCH_STRIDE bytes across [base, base + bytes),
 * so no two reads share a cache line on any machine whose lines are no
 * larger than the stride.  The opposite pattern to the sequential loop:
 * a memory that answers with a whole line gets nothing from it, because
 * nothing ever asks for the rest of what it fetched.
 *
 * The stride is a constant rather than an argument so the address
 * arithmetic stays immediate-only and the loop is the same instructions
 * on every machine; what varies per machine is the working set the
 * caller passes, sized from the cache geometry it discovered.
 *
 * `bytes` must be a multiple of MEMBENCH_STRIDE * 8 (the unroll).
 */
	.globl	membench_stride_ldw_use
	.type	membench_stride_ldw_use, @function
	.p2align 4
membench_stride_ldw_use:
	add	r2, r1
	mov	r3, r0
.Lstride_use:
	ldw	r4, [r1 + MEMBENCH_STRIDE * 0]
	xor	r3, r4
	ldw	r4, [r1 + MEMBENCH_STRIDE * 1]
	xor	r3, r4
	ldw	r4, [r1 + MEMBENCH_STRIDE * 2]
	xor	r3, r4
	ldw	r4, [r1 + MEMBENCH_STRIDE * 3]
	xor	r3, r4
	ldw	r4, [r1 + MEMBENCH_STRIDE * 4]
	xor	r3, r4
	ldw	r4, [r1 + MEMBENCH_STRIDE * 5]
	xor	r3, r4
	ldw	r4, [r1 + MEMBENCH_STRIDE * 6]
	xor	r3, r4
	ldw	r4, [r1 + MEMBENCH_STRIDE * 7]
	xor	r3, r4
	add	r1, MEMBENCH_STRIDE * 8
	cmp	r1, r2
	bne	.Lstride_use
	mov	r1, r3
	ret

/*
 * membench_stride_ldw_discard(uint32_t base, uint32_t bytes) -> 0
 *
 * The same reads as membench_stride_ldw_use, with nothing consuming them.
 */
	.globl	membench_stride_ldw_discard
	.type	membench_stride_ldw_discard, @function
	.p2align 4
membench_stride_ldw_discard:
	add	r2, r1
.Lstride_discard:
	ldw	r3,  [r1 + MEMBENCH_STRIDE * 0]
	ldw	r4,  [r1 + MEMBENCH_STRIDE * 1]
	ldw	r11, [r1 + MEMBENCH_STRIDE * 2]
	ldw	r3,  [r1 + MEMBENCH_STRIDE * 3]
	ldw	r4,  [r1 + MEMBENCH_STRIDE * 4]
	ldw	r11, [r1 + MEMBENCH_STRIDE * 5]
	ldw	r3,  [r1 + MEMBENCH_STRIDE * 6]
	ldw	r4,  [r1 + MEMBENCH_STRIDE * 7]
	add	r1, MEMBENCH_STRIDE * 8
	cmp	r1, r2
	bne	.Lstride_discard
	mov	r1, r0
	ret
