/*	$NetBSD$	*/
/*
 * Host unit test for the screen damage-diff (screen.c).
 *
 *   make test            (from sw/common)
 *   # or: cc -I. screen_test.c screen.c -o screen_test && ./screen_test
 *
 * Uses scr_init_mem() so it never touches a real terminal: scr_flush()
 * only fills the output buffer, which scr_last_output() hands back for
 * inspection.  The decisive property is frame 2 — an unchanged frame must
 * emit zero bytes, which is exactly what the diff buys over a repaint.
 */
#include "screen.h"

#include <stdio.h>

static int failures;

static void
check(int cond, const char *msg)
{
	printf("%s %s\n", cond ? "ok  " : "FAIL", msg);
	if (!cond)
		failures++;
}

static int
contains(const char *buf, size_t len, char c)
{
	size_t i;

	for (i = 0; i < len; i++)
		if (buf[i] == c)
			return 1;
	return 0;
}

static int
count(const char *buf, size_t len, char c)
{
	size_t i;
	int n = 0;

	for (i = 0; i < len; i++)
		if (buf[i] == c)
			n++;
	return n;
}

int
main(void)
{
	const scr_attr A = ATTR(C_DEFAULT, C_DEFAULT, 0);
	const char *out;
	size_t len;

	if (scr_init_mem(24, 80) < 0) {
		printf("FAIL scr_init_mem\n");
		return 1;
	}

	/* Frame 1: first paint of "HELLO" — must emit something. */
	scr_clear();
	scr_puts(0, 0, A, "HELLO");
	scr_flush();
	out = scr_last_output(&len);
	check(len > 0, "first frame emits bytes");
	check(contains(out, len, 'H'), "first frame contains the text");

	/* Frame 2: identical content — the diff must emit nothing. */
	scr_clear();
	scr_puts(0, 0, A, "HELLO");
	scr_flush();
	(void)scr_last_output(&len);
	check(len == 0, "unchanged frame emits zero bytes");

	/* Frame 3: one cell changes (O->P) — short, targeted update. */
	scr_clear();
	scr_puts(0, 0, A, "HELLP");
	scr_flush();
	out = scr_last_output(&len);
	check(len > 0 && len < 64, "single-cell change is a short update");
	check(contains(out, len, 'P'), "single-cell change emits the new glyph");

	/* Frame 4: unchanged content after scr_repaint() — the diff must
	 * redraw everything, since the terminal's contents are no longer
	 * what the front buffer claims (a kernel message overwrote them). */
	scr_repaint();
	scr_clear();
	scr_puts(0, 0, A, "HELLP");
	scr_flush();
	out = scr_last_output(&len);
	check(len > 0, "repaint forces output for unchanged content");
	check(contains(out, len, 'P'), "repaint re-emits the text");

	/*
	 * Frame 5: line drawing.  The glyph must reach the wire as the
	 * bare DEC Special Graphics character between a shift in and a
	 * shift back — never as UTF-8, which the console cannot decode.
	 */
	scr_repaint();
	scr_clear();
	scr_putc(scr_rows() - 1, scr_cols() - 1, GLYPH_HLINE, A);
	scr_flush();
	out = scr_last_output(&len);
	check(contains(out, len, 0x0e), "line drawing shifts G1 in");
	check(contains(out, len, 'q'), "line drawing emits the DEC character");
	check(!contains(out, len, (char)0xe2),
	    "line drawing emits no UTF-8 lead byte");

	/*
	 * The glyph sits in the last cell, so the shift back is the frame's
	 * own doing rather than the next cell's.  A frame that ended in G1
	 * would draw the following frame's text — and any shell that came
	 * after it — as line art.
	 */
	check(len > 0 && out[len - 1] == 0x0f, "the frame ends back in G0");

	/* Frame 6: a run of one character set costs one shift, not one
	 * per cell, the same way a run of one colour costs one SGR. */
	scr_repaint();
	scr_clear();
	scr_fill(0, 0, 40, GLYPH_HLINE, A);
	scr_flush();
	out = scr_last_output(&len);
	check(count(out, len, 0x0e) == 1, "a run of line drawing shifts once");

	printf(failures ? "\n%d FAILED\n" : "\nALL PASSED\n", failures);
	return failures ? 1 : 0;
}
