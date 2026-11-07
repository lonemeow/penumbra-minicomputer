/* dterm.h — terminal handling shared by the demos.
 *
 * The demos draw the same way: size themselves to the terminal, colour
 * cells from an xterm 256-colour palette, and emit escape sequences in a
 * tight loop.  Which colours a terminal actually has, and how wide it
 * is, differ between a serial terminal emulator and the wscons console.
 */

#ifndef DTERM_H
#define DTERM_H

/*
 * Size from the terminal, leaving *cols and *rows alone when it cannot
 * be determined, so a caller's own default survives being piped.
 * `above' and `below' rows are kept back for whatever the caller prints
 * around its picture, and are subtracted only from a measured size.
 * Positions are then relative to the picture, not the screen.
 * Also prepares the colour sequences below.
 */
void	dterm_init(int *cols, int *rows, int above, int below);

/*
 * Emit one cell holding two stacked pixels.  Where a half block can be
 * drawn both show; where it cannot the cell carries a background colour
 * only, so the brighter of the two is kept — dropping the top one
 * outright would erase half of a thin trace.
 */
char   *dterm_pair(char *out, int top, int bot);

/*
 * The same cell in two halves, for callers that repeat a colour across a
 * run and emit only the character per cell.
 */
char   *dterm_pair_color(char *out, int top, int bot);
char   *dterm_pair_glyph(char *out);

int	dterm_colors(void);

/*
 * Whether the terminal can render the block characters the demos use to
 * fit two pixels in a cell.  No terminfo capability describes this, so
 * it is answered by asking the terminal what it is.
 */
int	dterm_blocks(void);

/*
 * How the picture is actually being drawn, for the demos' banners: what
 * the terminal turned out to support, not what was asked for.
 */
const char *dterm_mode_name(int blocks);

/*
 * Append an SGR sequence for an xterm 256-colour index and return the
 * advanced write pointer.  The sequences are built once by dterm_init,
 * so the draw loop copies bytes and never formats a decimal; indices
 * beyond what the terminal supports are folded onto its nearest colour.
 */
char   *dterm_fg(char *out, int c);
char   *dterm_bg(char *out, int c);
char   *dterm_fgbg(char *out, int fg, int bg);

/* Move to a 1-based row and column, decimals precomputed like the
 * colours: the demos reposition once per row. */
char   *dterm_at(char *out, int row, int col);

void	dterm_cursor(int on);
void	dterm_clear(void);

/*
 * The palette's black.  The first sixteen indices are the terminal's
 * own colours and its "black" may be any dark shade, so the demos take
 * theirs from the cube, whose corner is black on every terminal.
 */
#define DTERM_BLACK	16

/*
 * Clear the screen and lay the ground for a picture of `cols' by `rows'
 * cells: DTERM_BLACK wherever a demo will draw.  A demo that touches
 * only some cells gives the empty half of each DTERM_BLACK and leaves
 * the rest alone, and the two meet without a seam only where the
 * untouched cells are that same black, which a terminal's own
 * background, being its theme's to choose, need not be.
 *
 * Leaves attributes off.  Written through stdio like the rest, so a
 * caller that draws with write(2) flushes before it does.
 */
void	dterm_canvas(int cols, int rows);
void	dterm_home(void);
void	dterm_reset(void);

/*
 * Confine the terminal's scrolling to the picture's first `rows' rows,
 * so a newline on the last of them moves the picture up a line and
 * leaves the rows reserved above and below it alone.  Setting a region
 * homes the cursor.  dterm_end lifts it again.
 */
char   *dterm_scroll_region(char *out, int rows);

/*
 * Finish with the picture: attributes off, any scroll region lifted,
 * and the cursor parked on the line below it, so whatever a demo
 * prints on the way out starts on a clean line instead of wherever the
 * last cell happened to land.
 */
void	dterm_end(void);

/* SIGINT is caught rather than fatal so a demo can restore the
 * terminal before leaving. */
void	dterm_catch_interrupt(void);
int	dterm_interrupted(void);

int	dterm_looks_like_int(const char *s);

#endif /* DTERM_H */
