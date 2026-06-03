/*	$NetBSD$	*/
/*
 * screen — a tiny virtual-screen / damage-diff terminal layer.
 *
 * Replaces libcurses for penmon.  The renderer draws into an off-screen
 * cell grid (the "back" buffer); scr_flush() diffs it against what is
 * already on the terminal (the "front" buffer) and emits only the
 * changed cells as one ANSI byte stream written with a single write(2).
 *
 * This keeps curses' cheap half — the virtual-screen diff — and drops
 * its expensive half: NetBSD libcurses flushes (hence write(2)s) once
 * per character, which is why penmon repainted so slowly over the
 * 115200 serial console.  One frame here is one syscall.
 *
 * Glyphs are emitted as raw UTF-8 bytes, so the result does not depend
 * on the *target's* locale (unlike curses' wide-char path) — only on
 * the host terminal being UTF-8, which modern emulators are.
 */
#ifndef PENMON_SCREEN_H
#define PENMON_SCREEN_H

#include <stddef.h>
#include <stdint.h>

/* ANSI colour indices: SGR foreground 30+idx, background 40+idx.
 * C_DEFAULT selects the terminal's own default (SGR 39/49). */
enum {
	C_BLACK = 0, C_RED, C_GREEN, C_YELLOW,
	C_BLUE, C_MAGENTA, C_CYAN, C_WHITE,
	C_DEFAULT = 9
};

/* A packed cell attribute: fg in bits [3:0], bg in [7:4], bold in [8]. */
typedef uint16_t scr_attr;

static inline scr_attr
ATTR(int fg, int bg, int bold)
{
	return (scr_attr)((fg & 0xf) | ((bg & 0xf) << 4) | ((bold & 1) << 8));
}

/* Box-drawing / shading glyphs (Unicode code points), emitted as UTF-8. */
#define GLYPH_BLOCK 0x2588u	/* █ full block      — bar fill        */
#define GLYPH_TRACK 0x2591u	/* ░ light shade     — empty bar track */
#define GLYPH_HLINE 0x2500u	/* ─ horizontal line — panel rules     */

/* ---- lifecycle -------------------------------------------------------- */

/* Enter raw mode + alternate screen + hidden cursor, size the buffers.
 * Returns 0 on success, -1 on failure (terminal left untouched). */
int  scr_init(void);

/* Restore the cursor, leave the alternate screen, restore termios. */
void scr_shutdown(void);

int  scr_rows(void);
int  scr_cols(void);

/* ---- drawing (into the back buffer) ----------------------------------- */

/* Begin a frame: clear the back buffer to blank cells. */
void scr_clear(void);

/* All coordinates are clipped to the screen; out-of-range writes vanish. */
void scr_putc(int y, int x, uint32_t ch, scr_attr a);
void scr_puts(int y, int x, scr_attr a, const char *s);
void scr_printf(int y, int x, scr_attr a, const char *fmt, ...)
	__attribute__((format(printf, 4, 5)));
void scr_fill(int y, int x, int w, uint32_t ch, scr_attr a);

/* Diff back vs front, emit the minimal ANSI as one write(2); front:=back. */
void scr_flush(void);

/* Re-query the terminal size; on change, resize the buffers and force a
 * full repaint on the next flush.  Returns 1 if the size changed. */
int  scr_resize(void);

/* ---- input ------------------------------------------------------------ */

/* Blocking single-byte read with a millisecond timeout.  Returns the
 * byte, or -1 on timeout / error. */
int  scr_getkey(int timeout_ms);

/* ---- testing hooks ---------------------------------------------------- */

/* Allocate buffers at a fixed size without touching the real terminal,
 * and suppress the write(2) so scr_flush() only fills the output buffer.
 * Used by the host unit test (screen_test.c). */
int         scr_init_mem(int rows, int cols);

/* Override the output fd (default the terminal; -1 suppresses write(2)). */
void        scr_set_outfd(int fd);

/* Bytes emitted by the most recent scr_flush(). */
const char *scr_last_output(size_t *len);

#endif /* PENMON_SCREEN_H */
