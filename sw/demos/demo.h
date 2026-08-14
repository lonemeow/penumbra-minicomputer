/* demo.h — shared runtime for the Penumbra graphics demos.
 *
 * A demo supplies visuals; everything around them belongs here.  The
 * runtime owns argument parsing, choosing and opening a surface,
 * reporting geometry, the frame loop and its stop conditions, holding a
 * finished picture, the performance-counter window, and teardown — so a
 * demo is its drawing plus a preset table.
 *
 * Two shapes, because the demos genuinely come in two kinds:
 *
 *   one-shot   compute a picture once, then hold it until dismissed.
 *              Mandelbrot and Julia.
 *   looped     advance state and draw repeatedly until stopped.
 *              Plasma, Lorenz, shadebobs, the logo screensaver.
 *
 * Two surfaces, not one behind a mux.  A cell grid and a pixel grid
 * differ in extent, aspect and cost model, and a common drawing API
 * over both would put a coordinate scale on the per-pixel path — which
 * on this hardware means a multiply or divide per pixel through the
 * divmul unit.  A demo that supports both provides a renderer for each,
 * and one that supports only cells degrades to them with no ceremony.
 */

#ifndef DEMO_H
#define DEMO_H

#include <stdint.h>

/* ── Surfaces ─────────────────────────────────────────────────────── */

enum demo_surface_kind {
	DEMO_CELL,	/* terminal character grid */
	DEMO_PIXEL,	/* framebuffer, 8bpp through a palette */
};

/*
 * What a demo needs to know about where it is drawing.  Extent is in
 * the surface's own units — cells or pixels — and the aspect is the
 * shape of one of those units, so geometry code can stay unit-agnostic
 * without asking which surface it has: terminal cells are 1 wide by 2
 * tall, framebuffer pixels are square.
 */
struct demo_surface {
	enum demo_surface_kind	kind;
	unsigned		width;
	unsigned		height;
	unsigned		aspect_w;	/* unit width  : 1 either way */
	unsigned		aspect_h;	/* unit height : 2 cells, 1 pixel */

	/* DEMO_PIXEL only.  Pixels are written here directly; this is the
	 * device mapping, so a store is the output and there is nothing
	 * to flush. */
	uint8_t			*pix;
	unsigned		stride;		/* bytes per row */
	unsigned		cmap_entries;

	/* DEMO_CELL only: true when the terminal can do the half-block
	 * trick, which doubles effective vertical resolution. */
	int			blocks;
};

/* Install a colormap on a pixel surface: three arrays of cmap_entries
 * bytes.  Ignored on a cell surface, where color comes from the
 * terminal's own palette. */
void	demo_set_cmap(const struct demo_surface *s,
	    const uint8_t *r, const uint8_t *g, const uint8_t *b);

/*
 * An xterm-256 palette index as 24-bit RGB.  A demo that picks its
 * colours from the terminal's fixed palette needs them as actual
 * values to fill a pixel surface's colormap, and the point is that
 * both surfaces then show the same thing.
 */
void	demo_xterm_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b);

/* ── The demo ─────────────────────────────────────────────────────── */

struct demo;

/*
 * Renderers.  A demo sets the pair it supports for each shape; a NULL
 * pixel renderer is how a demo says "cells only", and the runtime then
 * never selects a pixel surface for it.
 *
 * dt_us is microseconds since the previous frame *started*, so it
 * covers that frame's own cost and animation advances at a constant
 * wall-clock rate whichever surface is in use.  The first frame gets
 * zero, which is both the honest answer and easy to test for.
 */
typedef void (*demo_render_fn)(const struct demo_surface *s, void *state);
typedef void (*demo_frame_fn)(const struct demo_surface *s, uint32_t dt_us,
	    uint64_t frame, void *state);

struct demo {
	const char	*name;

	/* One-shot demos set render_*; looped demos set frame_*.  Setting
	 * both is a programming error the runtime rejects. */
	demo_render_fn	render_cell;
	demo_render_fn	render_pixel;
	demo_frame_fn	frame_cell;
	demo_frame_fn	frame_pixel;

	/*
	 * Called once before a surface is chosen, with the arguments the
	 * runtime did not consume, so a demo can read its own presets and
	 * sizes.  Returns 0 on success; anything else aborts the run after
	 * the demo has printed its own complaint.
	 */
	int		(*parse)(int argc, char **argv, void *state);

	/* Printed after the runtime's own options in a usage message. */
	const char	*usage_tail;

	/* Handed to every callback; the runtime never looks inside. */
	void		*state;
};

/*
 * Run a demo: parse arguments, choose and open a surface, render or
 * loop, hold, tear down.  Returns the process exit status.
 *
 * Surface choice, in order: an explicit flag wins; otherwise a pixel
 * surface is used when the demo has a pixel renderer and stdout is a
 * display offering one; otherwise cells.  Nothing about running on a
 * framebuffer needs to be asked for.
 */
int	demo_main(int argc, char **argv, const struct demo *d);

/* ── Geometry ─────────────────────────────────────────────────────── */

/*
 * A viewport in a demo's own coordinate space, mapped onto a surface
 * with its aspect accounted for, so circles are round on both.  Values
 * are whatever fixed-point format the caller uses: the helper only
 * scales and divides, and never interprets.
 */
struct demo_viewport {
	int32_t	x_min, y_min;	/* top-left */
	int32_t	dx, dy;		/* step per surface unit */
};

/*
 * sample_rows is how many rows the demo will actually sample, which is
 * not always the surface's height: the half-block trick puts two
 * samples in one cell.  The surface's aspect fixes the picture's
 * proportions, the sample count fixes the step between rows, and they
 * are separate numbers.
 */
struct demo_viewport demo_viewport(const struct demo_surface *s,
	    int32_t cx, int32_t cy, int32_t half_w, unsigned sample_rows);

#endif /* DEMO_H */
