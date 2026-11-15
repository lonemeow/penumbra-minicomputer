/*
 * loops.S — transfer loops for memspeed's block-size sweep.
 *
 * Hand-written so the instruction stream is the same on every build:
 * the numbers memspeed prints are rates of the memory system at each
 * block size, and a loop the compiler re-schedules would make them
 * rates of the compiler instead.  An external routine's loads cannot be
 * elided by a caller that cannot see them, and a load is never
 * architecturally dead in any case — it can fault, and it can be a
 * device register — so nothing removes the reads either.
 *
 * Every loop repeats its pass over the block `passes` times itself,
 * so a call is the whole measurement and the caller's timer brackets
 * it once.  A pass moves 128 bytes an iteration (BLOCK_GRANULE in
 * entry.c), eight 16-byte cache lines, which keeps the loop's own
 * control to three or four instructions in every 32 accesses.
 *
 * No loaded value is waited for:
 *
 *   - The read loop discards what it loads, into four registers in
 *     turn, so no two consecutive loads write the same register and
 *     nothing ever reads one.  It measures how fast the memory system
 *     delivers, not how fast the core consumes.
 *   - The write loop stores one register that never changes.
 *   - The copy loop orders each line's loads and stores so a store
 *     never waits on the load that fed it (COPY_LINE).
 *
 * ABI: r1-r4 the arguments, r1-r4 and r11 scratch, r5-r10 callee-saved
 * (doc/system/abi.md).  The loops that need more than the scratch
 * registers save what they use on the stack, outside the timed loop.
 */

	.equ	ITER_BYTES, 128

	.text

/* ── Read ─────────────────────────────────────────────────────────── */

/* Load one 16-byte line at r1 + off, discarding the words. */
	.macro	READ_LINE off
	ldw	r11, [r1 + \off]
	ldw	r5,  [r1 + (\off + 4)]
	ldw	r6,  [r1 + (\off + 8)]
	ldw	r7,  [r1 + (\off + 12)]
	.endm

/*
 * memspeed_read(uint32_t base, uint32_t bytes, uint32_t passes)
 *
 * Reads every word of [base, base + bytes) in ascending order, `passes`
 * times.  `bytes` is a non-zero multiple of ITER_BYTES and `passes`
 * non-zero.
 *
 * r1 cursor, r2 end, r3 passes left, r4 base.
 */
	.globl	memspeed_read
	.type	memspeed_read, @function
	.p2align 4
memspeed_read:
	sub	sp, #12
	stw	r5, [sp]
	stw	r6, [sp + 4]
	stw	r7, [sp + 8]
	mov	r4, r1
	add	r2, r1
.Lread_pass:
	mov	r1, r4
.Lread_iter:
	READ_LINE 0
	READ_LINE 16
	READ_LINE 32
	READ_LINE 48
	READ_LINE 64
	READ_LINE 80
	READ_LINE 96
	READ_LINE 112
	add	r1, #ITER_BYTES
	cmp	r1, r2
	bne	.Lread_iter
	sub	r3, #1
	bne	.Lread_pass
	ldw	r5, [sp]
	ldw	r6, [sp + 4]
	ldw	r7, [sp + 8]
	add	sp, #12
	ret
	.size	memspeed_read, . - memspeed_read

/* ── Write ────────────────────────────────────────────────────────── */

/* Store r4 to each word of the 16-byte line at r1 + off. */
	.macro	WRITE_LINE off
	stw	r4, [r1 + \off]
	stw	r4, [r1 + (\off + 4)]
	stw	r4, [r1 + (\off + 8)]
	stw	r4, [r1 + (\off + 12)]
	.endm

/*
 * memspeed_write(uint32_t base, uint32_t bytes, uint32_t passes,
 *                uint32_t value)
 *
 * Stores `value` to every word of [base, base + bytes) in ascending
 * order, `passes` times.  `bytes` is a non-zero multiple of ITER_BYTES
 * and `passes` non-zero.
 *
 * r1 cursor, r2 end, r3 passes left, r4 value, r11 base.
 */
	.globl	memspeed_write
	.type	memspeed_write, @function
	.p2align 4
memspeed_write:
	mov	r11, r1
	add	r2, r1
.Lwrite_pass:
	mov	r1, r11
.Lwrite_iter:
	WRITE_LINE 0
	WRITE_LINE 16
	WRITE_LINE 32
	WRITE_LINE 48
	WRITE_LINE 64
	WRITE_LINE 80
	WRITE_LINE 96
	WRITE_LINE 112
	add	r1, #ITER_BYTES
	cmp	r1, r2
	bne	.Lwrite_iter
	sub	r3, #1
	bne	.Lwrite_pass
	ret
	.size	memspeed_write, . - memspeed_write

/* ── Copy ─────────────────────────────────────────────────────────── */

/*
 * Copy the 16-byte line at r2 + off to r1 + off, through r5-r8.  The
 * four loads go first, so each store comes four instructions after
 * the load that fed it, past the load latency of every generation.
 */
	.macro	COPY_LINE off
	ldw	r5, [r2 + \off]
	ldw	r6, [r2 + (\off + 4)]
	ldw	r7, [r2 + (\off + 8)]
	ldw	r8, [r2 + (\off + 12)]
	stw	r5, [r1 + \off]
	stw	r6, [r1 + (\off + 4)]
	stw	r7, [r1 + (\off + 8)]
	stw	r8, [r1 + (\off + 12)]
	.endm

/*
 * memspeed_copy(uint32_t dst, uint32_t src, uint32_t bytes,
 *               uint32_t passes)
 *
 * Copies [src, src + bytes) to [dst, dst + bytes) in ascending order,
 * `passes` times.  `bytes` is a non-zero multiple of ITER_BYTES,
 * `passes` non-zero, and the two blocks do not overlap.
 *
 * r1 dst cursor, r2 src cursor, r3 bytes, r4 passes left, r11 src end.
 */
	.globl	memspeed_copy
	.type	memspeed_copy, @function
	.p2align 4
memspeed_copy:
	sub	sp, #16
	stw	r5, [sp]
	stw	r6, [sp + 4]
	stw	r7, [sp + 8]
	stw	r8, [sp + 12]
	mov	r11, r2
	add	r11, r3
.Lcopy_iter:
	COPY_LINE 0
	COPY_LINE 16
	COPY_LINE 32
	COPY_LINE 48
	COPY_LINE 64
	COPY_LINE 80
	COPY_LINE 96
	COPY_LINE 112
	add	r2, #ITER_BYTES
	add	r1, #ITER_BYTES
	cmp	r2, r11
	bne	.Lcopy_iter
	sub	r1, r3
	sub	r2, r3
	sub	r4, #1
	bne	.Lcopy_iter
	ldw	r5, [sp]
	ldw	r6, [sp + 4]
	ldw	r7, [sp + 8]
	ldw	r8, [sp + 12]
	add	sp, #16
	ret
	.size	memspeed_copy, . - memspeed_copy
