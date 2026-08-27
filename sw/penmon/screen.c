/*	$NetBSD$	*/
/*
 * screen — virtual-screen / damage-diff terminal layer (see screen.h).
 *
 * Two cell grids: `back` (drawn this frame) and `front` (on screen now).
 * scr_flush() walks them row by row, appends the minimal ANSI to a
 * growable byte buffer, writes it with one write(2), then makes front
 * match back.  All control output goes through this buffer so a frame is
 * a single syscall.
 */
#include "screen.h"

#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

struct cell {
	uint16_t ch;		/* Unicode code point; penmon's glyphs are <= U+2591 */
	scr_attr attr;		/* packed fg/bg/bold — 4-byte cell, no padding */
};

static int rows, cols;
static struct cell *back, *front;

static char  *outbuf;		/* assembled frame, flushed in one write(2) */
static size_t outcap, outlen;
static scr_attr cur_attr;	/* SGR currently asserted while flushing */

static int out_fd = -1;		/* terminal fd; -1 suppresses write(2) */
static struct termios saved_tio;
static int tio_saved;

/* ---- output buffer helpers -------------------------------------------- */

static int
out_reserve(size_t extra)
{
	char *p;
	size_t ncap;

	if (outlen + extra <= outcap)
		return 0;
	ncap = outcap ? outcap : 4096;
	while (ncap < outlen + extra)
		ncap *= 2;
	if ((p = realloc(outbuf, ncap)) == NULL)
		return -1;
	outbuf = p;
	outcap = ncap;
	return 0;
}

static void
out_raw(const char *s, size_t n)
{
	if (out_reserve(n) == 0) {
		memcpy(outbuf + outlen, s, n);
		outlen += n;
	}
}

static void
out_byte(unsigned char b)
{
	if (out_reserve(1) == 0)
		outbuf[outlen++] = (char)b;
}

/* Append a code point as UTF-8 (penmon's glyphs reach U+2588). */
static void
emit_utf8(uint32_t cp)
{
	if (cp < 0x80) {
		out_byte((unsigned char)cp);
	} else if (cp < 0x800) {
		out_byte(0xc0 | (cp >> 6));
		out_byte(0x80 | (cp & 0x3f));
	} else {
		out_byte(0xe0 | (cp >> 12));
		out_byte(0x80 | ((cp >> 6) & 0x3f));
		out_byte(0x80 | (cp & 0x3f));
	}
}

/* Append an SGR sequence only when the attribute actually changes. */
static void
emit_attr(scr_attr a)
{
	int fg, bg, bold, fc, bc, n;
	char t[24];

	if (a == cur_attr)
		return;
	fg = a & 0xf;
	bg = (a >> 4) & 0xf;
	bold = (a >> 8) & 1;
	fc = (fg == C_DEFAULT) ? 39 : 30 + fg;
	bc = (bg == C_DEFAULT) ? 49 : 40 + bg;
	if (bold)
		n = snprintf(t, sizeof t, "\x1b[0;1;%d;%dm", fc, bc);
	else
		n = snprintf(t, sizeof t, "\x1b[0;%d;%dm", fc, bc);
	out_raw(t, (size_t)n);
	cur_attr = a;
}

/* Append a cursor-position escape (0-based row/col -> 1-based ANSI). */
static void
emit_move(int y, int x)
{
	char t[24];
	int n = snprintf(t, sizeof t, "\x1b[%d;%dH", y + 1, x + 1);

	out_raw(t, (size_t)n);
}

/* Append one cell: its attribute (if changed) then its glyph. */
static void
emit_cell(const struct cell *c)
{
	emit_attr(c->attr);
	emit_utf8(c->ch);
}

/* True when two cells would render identically. */
static inline int
cell_eq(const struct cell *a, const struct cell *b)
{
	return a->ch == b->ch && a->attr == b->attr;
}

/* ---- the damage diff -------------------------------------------------- */

/*
 * flush_row — emit the minimal ANSI to make on-screen row `y` (front)
 * match the freshly drawn row `y` (back).
 *
 * back  = &back [y * cols .. +cols)   what we want on screen
 * front = &front[y * cols .. +cols)   what is on screen right now
 *
 * Helpers available: cell_eq(a,b), emit_move(y,x), emit_cell(&cell).
 * The caller copies back -> front after every row, so DO NOT touch
 * front here.  The cursor position is unknown on entry, so emit_move()
 * before the first cell you draw.
 */
static void
flush_row(int y)
{
	const struct cell *b = &back[y * cols];
	const struct cell *f = &front[y * cols];
	int x = 0;

	while (1) {
		while (x < cols && cell_eq(&b[x], &f[x]))
			x++;
		
		if (x >= cols)
			break;
		
		emit_move(y, x);
		while (x < cols && !cell_eq(&b[x], &f[x]))
			emit_cell(&b[x++]);
	};
}

void
scr_flush(void)
{
	int y;

	outlen = 0;
	cur_attr = 0xffff;	/* unknown: first emit_attr() re-asserts SGR */
	for (y = 0; y < rows; y++)
		flush_row(y);
	if (outlen > 0 && out_fd >= 0)
		(void)write(out_fd, outbuf, outlen);
	if (rows > 0 && cols > 0)
		memcpy(front, back, (size_t)rows * cols * sizeof(struct cell));
}

/* ---- drawing ---------------------------------------------------------- */

void
scr_clear(void)
{
	scr_attr a = ATTR(C_DEFAULT, C_DEFAULT, 0);
	int i, n = rows * cols;

	for (i = 0; i < n; i++) {
		back[i].ch = ' ';
		back[i].attr = a;
	}
}

void
scr_putc(int y, int x, uint32_t ch, scr_attr a)
{
	if (y < 0 || y >= rows || x < 0 || x >= cols)
		return;
	back[y * cols + x].ch = ch;
	back[y * cols + x].attr = a;
}

void
scr_puts(int y, int x, scr_attr a, const char *s)
{
	for (; *s != '\0' && x < cols; x++, s++)
		scr_putc(y, x, (unsigned char)*s, a);
}

void
scr_printf(int y, int x, scr_attr a, const char *fmt, ...)
{
	char buf[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	scr_puts(y, x, a, buf);
}

void
scr_fill(int y, int x, int w, uint32_t ch, scr_attr a)
{
	int i;

	for (i = 0; i < w; i++)
		scr_putc(y, x + i, ch, a);
}

/* ---- buffers / sizing ------------------------------------------------- */

static int
alloc_buffers(int r, int c)
{
	size_t n = (size_t)r * c;
	struct cell *nb, *nf;
	size_t i;

	nb = malloc(n * sizeof *nb);
	nf = malloc(n * sizeof *nf);
	if (nb == NULL || nf == NULL) {
		free(nb);
		free(nf);
		return -1;
	}
	free(back);
	free(front);
	back = nb;
	front = nf;
	rows = r;
	cols = c;
	for (i = 0; i < n; i++) {
		back[i].ch = ' ';
		back[i].attr = ATTR(C_DEFAULT, C_DEFAULT, 0);
		front[i].ch = 0xffffu;		/* impossible: forces a */
		front[i].attr = 0xffff;		/* full first repaint   */
	}
	return 0;
}

static void
query_size(int *r, int *c)
{
	struct winsize ws;
	const char *e;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
	    ws.ws_row > 0 && ws.ws_col > 0) {
		*r = ws.ws_row;
		*c = ws.ws_col;
		return;
	}
	*r = 24;
	*c = 80;				/* serial console fallback */
	if ((e = getenv("LINES")) != NULL && atoi(e) > 0)
		*r = atoi(e);
	if ((e = getenv("COLUMNS")) != NULL && atoi(e) > 0)
		*c = atoi(e);
}

/* Write a control string straight to the terminal (setup/teardown). */
static void
term_write(const char *s)
{
	if (out_fd >= 0)
		(void)write(out_fd, s, strlen(s));
}

int  scr_rows(void) { return rows; }
int  scr_cols(void) { return cols; }

int
scr_init(void)
{
	struct termios t;
	int r, c;

	out_fd = STDOUT_FILENO;
	query_size(&r, &c);
	if (alloc_buffers(r, c) < 0)
		return -1;
	if (tcgetattr(STDIN_FILENO, &saved_tio) == 0) {
		t = saved_tio;
		t.c_lflag &= ~(ICANON | ECHO);	/* keep ISIG: Ctrl-C quits */
		t.c_cc[VMIN] = 0;
		t.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &t);
		tio_saved = 1;
	}
	/* Alt screen where it exists, then an explicit clear and home:
	 * terminals without the alternate buffer ignore the first
	 * sequence, and the display must start from a blank screen and a
	 * known cursor position either way. */
	term_write("\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H");
	return 0;
}

void
scr_shutdown(void)
{
	/* Leaving the alt screen restores what was there before; without
	 * one, clear and home so the shell prompt returns to a clean
	 * screen instead of the last frame. */
	term_write("\x1b[0m\x1b[?25h\x1b[?1049l\x1b[2J\x1b[H");
	if (tio_saved) {
		tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
		tio_saved = 0;
	}
	free(back);
	free(front);
	free(outbuf);
	back = front = NULL;
	outbuf = NULL;
	rows = cols = 0;
	outcap = outlen = 0;
}

void
scr_repaint(void)
{
	int i, n = rows * cols;

	for (i = 0; i < n; i++) {
		front[i].ch = 0xffffu;		/* impossible: every cell */
		front[i].attr = 0xffff;		/* differs from the back  */
	}
	term_write("\x1b[2J\x1b[H");
}

int
scr_resize(void)
{
	int r, c;

	query_size(&r, &c);
	if (r == rows && c == cols)
		return 0;
	if (alloc_buffers(r, c) < 0)
		return 0;
	term_write("\x1b[2J");		/* clear; impossible front -> full repaint */
	return 1;
}

/* ---- input ------------------------------------------------------------ */

int
scr_getkey(int timeout_ms)
{
	struct pollfd p;
	unsigned char ch;

	p.fd = STDIN_FILENO;
	p.events = POLLIN;
	p.revents = 0;
	if (poll(&p, 1, timeout_ms) <= 0)
		return -1;
	if (read(STDIN_FILENO, &ch, 1) != 1)
		return -1;
	return (int)ch;
}

/* ---- testing hooks ---------------------------------------------------- */

int
scr_init_mem(int r, int c)
{
	out_fd = -1;
	return alloc_buffers(r, c);
}

void
scr_set_outfd(int fd)
{
	out_fd = fd;
}

int
scr_row_text(int y, char *buf, size_t bufsz)
{
	int x, n = 0;

	if (buf == NULL || bufsz == 0)
		return 0;
	buf[0] = '\0';
	if (y < 0 || y >= rows)
		return 0;
	for (x = 0; x < cols && (size_t)n + 1 < bufsz; x++) {
		uint32_t ch = back[y * cols + x].ch;

		buf[n++] = (ch >= ' ' && ch < 0x7f) ? (char)ch : '?';
	}
	buf[n] = '\0';
	return n;
}

const char *
scr_last_output(size_t *len)
{
	if (len != NULL)
		*len = outlen;
	return outbuf;
}
