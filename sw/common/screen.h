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
 * Glyphs outside ASCII are emitted from DEC Special Graphics rather
 * than as UTF-8, so the byte stream does not depend on a locale at
 * either end — only on the terminal being VT100-family, which the
 * NetBSD console and every serial terminal emulator both are.
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

/*
 * Glyphs past ASCII come from DEC Special Graphics: the line-drawing
 * set is designated into G1 once at init and reached by shifting, which
 * costs one byte per run and draws correctly on a serial terminal and
 * on the console alike.  UTF-8 would draw on neither reliably — the
 * console's emulator is byte-oriented and never decodes it.
 *
 * ACS() names any character of the set (`x` is a vertical line, `lqk`
 * and `mqj` the box corners); the two drawn with here have names of
 * their own.  The set has no full block, so a solid run is a space
 * carrying ATTR_SOLID() rather than a glyph.
 */
#define SCR_ACS     0x100u	/* low byte is a DEC Special Graphics char */
#define ACS(c)      (SCR_ACS | (unsigned char)(c))

#define GLYPH_HLINE ACS('q')	/* ─ horizontal line — panel rules       */
#define GLYPH_SHADE ACS('a')	/* ▒ checkerboard    — half-lit bar cell */

/* A cell rendering as a solid block of `color`, for the bar fill the
 * character set cannot supply. */
static inline scr_attr
ATTR_SOLID(int color)
{
	return ATTR(C_DEFAULT, color, 0);
}

/* ---- lifecycle -------------------------------------------------------- */

/* Enter raw mode + alternate screen + hidden cursor, size the buffers.
 * Returns 0 on success, -1 on failure (terminal left untouched). */
int  scr_init(void);

/* The same, drawing on an already-open terminal rather than stdout and
 * sizing itself from that one.  Keys still come from stdin, so a
 * display can be put on the console from a session elsewhere. */
int  scr_init_on(int outfd);

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

/* Discard what the terminal is believed to show, so the next flush
 * redraws every cell.  The console is shared — kernel messages land on
 * top of the display — and the diff cannot see that damage. */
void scr_repaint(void);

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

/* Copy row y of the freshly drawn screen into buf as NUL-terminated
 * ASCII, standing in non-ASCII glyphs (bars, box drawing) with '?'.
 * Lets a test assert on a laid-out row without parsing the ANSI stream.
 * Returns the number of characters written. */
int         scr_row_text(int y, char *buf, size_t bufsz);

/* The attribute of one cell of the freshly drawn screen, so a test can
 * assert on colour and not just on text.  Off-screen reads give 0. */
scr_attr    scr_cell_attr(int y, int x);

#endif /* PENMON_SCREEN_H */
