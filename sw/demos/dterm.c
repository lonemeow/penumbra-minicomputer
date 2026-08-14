/* dterm.c — terminal handling shared by the demos. */

#include <sys/ioctl.h>

#if defined(__has_include)
#  if __has_include(<dev/wscons/wsconsio.h>)
#    include <dev/wscons/wsconsio.h>
#    define DT_HAVE_WSCONS 1
#  endif
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <term.h>
#include <termios.h>
#include <unistd.h>

#include "dterm.h"

#define DT_NCOLORS	256		/* palette indices callers may use */
#define DT_SEQMAX	16		/* longest SGR sequence built here */

struct seq {
	char		s[DT_SEQMAX];
	unsigned char	len;
};

static struct seq	fg_seq[DT_NCOLORS];
static struct seq	bg_seq[DT_NCOLORS];
static struct seq	dec_seq[DT_NCOLORS];  /* row/column numbers */
static int		ncolors = 8;
static int		blocks_ok = 1;
static int		row_offset;	/* rows above the picture */
static int		pic_rows;	/* rows the picture occupies */
static volatile sig_atomic_t interrupted;

/*
 * xterm's 256-colour palette: 16 system colours, a 6x6x6 cube, then a
 * 24-step grey ramp.  Needed to fold an index onto a terminal that has
 * fewer colours, which means comparing them as RGB.
 */
static void
index_rgb(int c, int *r, int *g, int *b)
{
	static const unsigned char cube[6] = { 0, 95, 135, 175, 215, 255 };
	static const unsigned char sys[16][3] = {
		{   0,   0,   0 }, { 170,   0,   0 }, {   0, 170,   0 },
		{ 170,  85,   0 }, {   0,   0, 170 }, { 170,   0, 170 },
		{   0, 170, 170 }, { 170, 170, 170 }, {  85,  85,  85 },
		{ 255,  85,  85 }, {  85, 255,  85 }, { 255, 255,  85 },
		{  85,  85, 255 }, { 255,  85, 255 }, {  85, 255, 255 },
		{ 255, 255, 255 },
	};

	if (c < 16) {
		*r = sys[c][0]; *g = sys[c][1]; *b = sys[c][2];
	} else if (c < 232) {
		c -= 16;
		*r = cube[(c / 36) % 6];
		*g = cube[(c / 6) % 6];
		*b = cube[c % 6];
	} else {
		*r = *g = *b = 8 + (c - 232) * 10;
	}
}

/* Nearest of the first `n' ANSI colours, by squared RGB distance. */
static int
fold_color(int c, int n)
{
	int r, g, b, best = 0;
	long bestd = -1;

	index_rgb(c, &r, &g, &b);
	for (int i = 0; i < n; i++) {
		int ir, ig, ib;
		long d;

		index_rgb(i, &ir, &ig, &ib);
		d = (long)(r - ir) * (r - ir) + (long)(g - ig) * (g - ig) +
		    (long)(b - ib) * (b - ib);
		if (bestd < 0 || d < bestd) {
			bestd = d;
			best = i;
		}
	}
	return best;
}

/*
 * Build one colour sequence.  A terminal with only eight colours still
 * reaches the bright half through bold, and bright black is what makes
 * greys land on a grey rather than on the nearest hue, so the intensity
 * is folded into the same sequence.  Backgrounds have no bold, so they
 * fold onto the eight.
 */
static void
build_seq(struct seq *dst, const char *cap, int base, int c, int is_fg)
{
	char buf[DT_SEQMAX];
	const char *s = NULL;
	int idx;

	if (ncolors >= 256)
		idx = c;
	else
		idx = fold_color(c, (is_fg && ncolors < 16) ? 16 : 8);

	if (ncolors >= 16) {
		if (cap != NULL)
			s = tiparm(cap, idx);
		if (s == NULL) {
			(void)snprintf(buf, sizeof(buf), "\033[%dm",
			    base + (idx & 7));
			s = buf;
		}
	} else {
		/* Bold carries the bright half, so every sequence states the
		 * intensity or a dim colour would inherit the last bright
		 * one's bold. */
		(void)snprintf(buf, sizeof(buf), "\033[%d;%dm",
		    idx >= 8 ? 1 : 22, base + (idx & 7));
		s = buf;
	}

	dst->len = (unsigned char)strlen(s);
	if (dst->len > DT_SEQMAX)
		dst->len = DT_SEQMAX;
	memcpy(dst->s, s, dst->len);
}

void
dterm_init(int *cols, int *rows, int above, int below)
{
	struct winsize ws;
	const char *setaf = NULL, *setab = NULL, *e;
	int err;

	if (setupterm(NULL, STDOUT_FILENO, &err) == 0) {
		int n = tigetnum("colors");

		if (n >= 8)
			ncolors = n;
		setaf = tigetstr("setaf");
		setab = tigetstr("setab");
		if (setaf == (char *)-1) setaf = NULL;
		if (setab == (char *)-1) setab = NULL;
	}

	for (int c = 0; c < DT_NCOLORS; c++) {
		build_seq(&fg_seq[c], setaf, 30, c, 1);
		build_seq(&bg_seq[c], setab, 40, c, 0);
		dec_seq[c].len = (unsigned char)snprintf(dec_seq[c].s,
		    sizeof(dec_seq[c].s), "%d", c);
	}

#ifdef DT_HAVE_WSCONS
	/*
	 * A wscons console answers this and nothing else does.  Its
	 * emulation decodes no UTF-8 and can select no character set
	 * holding block elements, so the half-block trick cannot work
	 * there however the glyphs are encoded.
	 */
	{
		u_int wstype;

		if (ioctl(STDOUT_FILENO, WSDISPLAYIO_GTYPE, &wstype) == 0)
			blocks_ok = 0;
	}
#endif

	/* The kernel knows this screen; terminfo only knows the type, and
	 * says 25 lines for a console that is 30. */
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 &&
	    ws.ws_col > 0 && ws.ws_row > 0) {
		*cols = ws.ws_col;
		*rows = ws.ws_row - above - below;
	} else {
		if ((e = getenv("COLUMNS")) != NULL && atoi(e) > 0)
			*cols = atoi(e);
		if ((e = getenv("LINES")) != NULL && atoi(e) > 0)
			*rows = atoi(e) - above - below;
	}
	if (*rows < 1)
		*rows = 1;
	row_offset = above;
	pic_rows = *rows;
}

int
dterm_colors(void)
{
	return ncolors;
}

int
dterm_blocks(void)
{
	return blocks_ok;
}

/* Relative brightness of a palette index, for choosing between two. */
static int
luma(int c)
{
	int r, g, b;

	index_rgb(c, &r, &g, &b);
	return 2 * r + 4 * g + b;
}

char *
dterm_pair_color(char *out, int top, int bot)
{
	if (blocks_ok)
		return dterm_fgbg(out, top, bot);
	return dterm_bg(out, luma(top) >= luma(bot) ? top : bot);
}

char *
dterm_pair_glyph(char *out)
{
	if (blocks_ok) {
		memcpy(out, "\xe2\x96\x80", 3);	/* U+2580 UPPER HALF BLOCK */
		return out + 3;
	}
	*out++ = ' ';
	return out;
}

char *
dterm_pair(char *out, int top, int bot)
{
	return dterm_pair_glyph(dterm_pair_color(out, top, bot));
}

const char *
dterm_mode_name(int blocks)
{
	const char *shape = blocks_ok ? "blocks" : "cells";

	if (!blocks)
		return "ascii";
	if (ncolors >= 256)
		return blocks_ok ? "blocks, 256-color" : "cells, 256-color";
	if (ncolors >= 16)
		return blocks_ok ? "blocks, 16-color" : "cells, 16-color";
	(void)shape;
	return blocks_ok ? "blocks, 8-color" : "cells, 8-color";
}

char *
dterm_fg(char *out, int c)
{
	const struct seq *s = &fg_seq[c & (DT_NCOLORS - 1)];

	memcpy(out, s->s, s->len);
	return out + s->len;
}

char *
dterm_bg(char *out, int c)
{
	const struct seq *s = &bg_seq[c & (DT_NCOLORS - 1)];

	memcpy(out, s->s, s->len);
	return out + s->len;
}

char *
dterm_fgbg(char *out, int fg, int bg)
{
	return dterm_bg(dterm_fg(out, fg), bg);
}

char *
dterm_at(char *out, int row, int col)
{
	const struct seq *r = &dec_seq[(row + row_offset) & (DT_NCOLORS - 1)];
	const struct seq *c = &dec_seq[col & (DT_NCOLORS - 1)];

	*out++ = '\033';
	*out++ = '[';
	memcpy(out, r->s, r->len); out += r->len;
	*out++ = ';';
	memcpy(out, c->s, c->len); out += c->len;
	*out++ = 'H';
	return out;
}

void
dterm_cursor(int on)
{
	fputs(on ? "\033[?25h" : "\033[?25l", stdout);
}

void
dterm_clear(void)
{
	fputs("\033[2J", stdout);
}

/* Top-left of the picture, which is below whatever the caller reserved. */
void
dterm_home(void)
{
	printf("\033[%d;1H", row_offset + 1);
}

void
dterm_reset(void)
{
	fputs("\033[0m", stdout);
}

void
dterm_end(void)
{
	/* Land on the picture's last row and step off it, which scrolls by
	 * one when the picture reaches the bottom of the screen. */
	printf("\033[0m\033[%d;1H\n", row_offset + pic_rows);
}

static void
on_interrupt(int sig)
{
	(void)sig;
	interrupted = 1;
}

void
dterm_catch_interrupt(void)
{
	(void)signal(SIGINT, on_interrupt);
}

int
dterm_interrupted(void)
{
	return interrupted;
}

int
dterm_looks_like_int(const char *s)
{
	if (*s == '-' || *s == '+')
		s++;
	if (*s == '\0')
		return 0;
	for (; *s != '\0'; s++)
		if (*s < '0' || *s > '9')
			return 0;
	return 1;
}
