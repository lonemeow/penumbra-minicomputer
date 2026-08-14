/* demo.c — shared runtime for the Penumbra graphics demos.
 *
 * See demo.h for the shape of the thing.  This file is everything a
 * demo would otherwise repeat: options, surface lifecycle, the frame
 * loop and what stops it, the hold, the counter window, teardown.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>

#include <dev/wscons/wsconsio.h>

#include "demo.h"
#include "dterm.h"
#include "perfctr.h"

/* ── Stop conditions ──────────────────────────────────────────────── */
/*
 * One place owns stopping.  A demo never sees a signal handler or a
 * stdin read: it draws, and the runtime decides whether there is
 * another frame.  Keeping both mechanisms here is also what stops them
 * disagreeing — SIGINT and a keypress mean the same thing to a viewer.
 */
static volatile sig_atomic_t stop_requested;

static void
on_interrupt(int sig)
{
	(void)sig;
	stop_requested = 1;
}

/* Non-blocking check for a keypress, which the runtime consumes: a
 * viewer pressing a key to end a demo should not then find that key
 * queued for whatever runs next. */
static int
key_pressed(void)
{
	struct timeval tv = { 0, 0 };
	fd_set rfds;
	unsigned char c;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);
	if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, &tv) <= 0)
		return 0;
	if (read(STDIN_FILENO, &c, 1) != 1)
		return 0;
	return 1;
}

static uint64_t
now_us(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* ── Terminal mode ────────────────────────────────────────────────── */
/*
 * Single keypresses, not lines.  Restored on the way out so a demo that
 * ends leaves a usable shell behind.
 */
static struct termios	saved_tio;
static int		tio_saved;

static void
raw_input(void)
{
	struct termios raw;

	if (tcgetattr(STDIN_FILENO, &saved_tio) != 0)
		return;
	tio_saved = 1;
	raw = saved_tio;
	raw.c_lflag &= ~((tcflag_t)(ICANON | ECHO));
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	tcsetattr(STDIN_FILENO, TCSANOW, &raw);
}

static void
restore_input(void)
{
	if (tio_saved)
		tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio);
	tio_saved = 0;
}

/* ── Pixel surface ────────────────────────────────────────────────── */

static int	pixel_fd = -1;
static int	pixel_own_fd;
static int	pixel_mode_set;
static size_t	pixel_size;

/*
 * Open the framebuffer behind an fd, filling in the surface.  Returns 0
 * on success.  path NULL means stdout's own device, which is where a
 * picture belongs unless someone asked otherwise.
 */
static int
pixel_open(struct demo_surface *s, const char *path, int quiet)
{
	struct wsdisplayio_fbinfo fbi;
	const char *what = path ? path : "stdout";
	u_int mode;

	if (path == NULL) {
		pixel_fd = fileno(stdout);
	} else {
		/* O_NOCTTY: a display device, not a terminal to talk on. */
		pixel_fd = open(path, O_RDWR | O_NOCTTY);
		if (pixel_fd < 0) {
			fprintf(stderr, "%s: %s\n", path, strerror(errno));
			return -1;
		}
		pixel_own_fd = 1;
	}

	if (ioctl(pixel_fd, WSDISPLAYIO_GET_FBINFO, &fbi) != 0) {
		if (!quiet)
			fprintf(stderr, "%s has no framebuffer: %s\n",
			    what, strerror(errno));
		goto fail;
	}
	if (fbi.fbi_bitsperpixel != 8 || fbi.fbi_pixeltype != WSFB_CI) {
		if (!quiet)
			fprintf(stderr, "%s: unsupported format (%u bpp, "
			    "pixeltype %u)\n", what, fbi.fbi_bitsperpixel,
			    fbi.fbi_pixeltype);
		goto fail;
	}

	mode = WSDISPLAYIO_MODE_DUMBFB;
	if (ioctl(pixel_fd, WSDISPLAYIO_SMODE, &mode) != 0) {
		if (!quiet)
			fprintf(stderr, "%s: SMODE: %s\n", what,
			    strerror(errno));
		goto fail;
	}
	pixel_mode_set = 1;

	pixel_size = (size_t)fbi.fbi_fbsize;
	s->pix = mmap(NULL, pixel_size, PROT_READ | PROT_WRITE, MAP_SHARED,
	    pixel_fd, 0);
	if (s->pix == MAP_FAILED) {
		if (!quiet)
			fprintf(stderr, "%s: mmap: %s\n", what, strerror(errno));
		s->pix = NULL;
		goto fail;
	}

	s->kind = DEMO_PIXEL;
	s->width = fbi.fbi_width;
	s->height = fbi.fbi_height;
	s->stride = fbi.fbi_stride;
	s->cmap_entries = fbi.fbi_subtype.fbi_cmapinfo.cmap_entries;
	if (s->cmap_entries > 256)
		s->cmap_entries = 256;
	s->aspect_w = s->aspect_h = 1;	/* the device doubles both axes */

	/* Contents are undefined until written, and the screen is already
	 * showing this source.  A render takes long enough that without a
	 * clear the viewer watches the picture eat its way through
	 * whatever was there — on a cold boot, noise. */
	memset(s->pix, 0, pixel_size);
	return 0;

fail:
	if (pixel_mode_set) {
		mode = WSDISPLAYIO_MODE_EMUL;
		ioctl(pixel_fd, WSDISPLAYIO_SMODE, &mode);
		pixel_mode_set = 0;
	}
	if (pixel_own_fd)
		close(pixel_fd);
	pixel_fd = -1;
	pixel_own_fd = 0;
	return -1;
}

static void
pixel_close(struct demo_surface *s)
{
	u_int mode = WSDISPLAYIO_MODE_EMUL;

	if (s->pix != NULL)
		munmap(s->pix, pixel_size);
	if (pixel_mode_set)
		ioctl(pixel_fd, WSDISPLAYIO_SMODE, &mode);
	if (pixel_own_fd)
		close(pixel_fd);
	s->pix = NULL;
}

void
demo_set_cmap(const struct demo_surface *s, const uint8_t *r,
    const uint8_t *g, const uint8_t *b)
{
	struct wsdisplay_cmap cm;

	if (s->kind != DEMO_PIXEL || pixel_fd < 0)
		return;
	cm.index = 0;
	cm.count = s->cmap_entries;
	cm.red = (u_char *)(uintptr_t)r;
	cm.green = (u_char *)(uintptr_t)g;
	cm.blue = (u_char *)(uintptr_t)b;
	if (ioctl(pixel_fd, WSDISPLAYIO_PUTCMAP, &cm) != 0)
		fprintf(stderr, "PUTCMAP: %s\n", strerror(errno));
}

/* ── Geometry ─────────────────────────────────────────────────────── */

struct demo_viewport
demo_viewport(const struct demo_surface *s, int32_t cx, int32_t cy,
    int32_t half_w, unsigned sample_rows)
{
	/* half_h = half_w * (height * aspect_h) / (width * aspect_w) keeps
	 * the picture's proportions on a surface whose units are not
	 * square.  Done in 64-bit because half_w is already a fixed-point
	 * value using most of its range. */
	int64_t units_h = (int64_t)s->height * s->aspect_h;
	int64_t units_w = (int64_t)s->width * s->aspect_w;
	struct demo_viewport v;
	int32_t half_h;

	/* Every divisor below comes from a surface extent, so a degenerate
	 * one is a divide by zero rather than a small picture — and on
	 * this hardware that is a fault, not a NaN.  Refusing here keeps
	 * the failure at the one place that can see it coming. */
	if (units_w == 0 || s->width < 2 || sample_rows < 2) {
		memset(&v, 0, sizeof(v));
		return v;
	}

	half_h = (int32_t)(((int64_t)half_w * units_h) / units_w);
	v.x_min = cx - half_w;
	v.y_min = cy - half_h;
	v.dx = (int32_t)(((int64_t)half_w * 2) / (s->width - 1));
	v.dy = (int32_t)(((int64_t)half_h * 2) / (sample_rows - 1));
	return v;
}

/* ── Options ──────────────────────────────────────────────────────── */

enum surface_pref {
	PREF_AUTO,
	PREF_CELL,
	PREF_PIXEL,
};

struct demo_opts {
	enum surface_pref	pref;
	const char		*pixel_path;	/* NULL => stdout's device */
	int			mono;		/* force ASCII over blocks */
	int			hold_seconds;	/* one-shot: 0 waits for a key */
	int			run_seconds;	/* looped: 0 runs until stopped */
	/* Cell-surface extent override.  A pixel surface takes its size
	 * from the device and ignores this. */
	int			cols, rows;
};

static void
usage(const char *prog, const struct demo *d)
{
	fprintf(stderr,
	    "usage: %s [-b|--blocks | -m|--mono | -f|--fb [DEV]]\n"
	    "       %*s [-s|--size WxH] [-H|--hold SECONDS]\n"
	    "       %*s [-t|--run-for SECONDS]%s%s\n",
	    prog, (int)strlen(prog), "", (int)strlen(prog), "",
	    d->usage_tail ? " " : "", d->usage_tail ? d->usage_tail : "");
}

/*
 * Consume the runtime's own options from the front of argv, leaving the
 * rest for the demo.  Returns the index where the demo's arguments
 * begin, or -1 on a bad option.
 */
static int
parse_opts(int argc, char **argv, struct demo_opts *o)
{
	int i = 1;

	while (i < argc) {
		const char *a = argv[i];

		if (strcmp(a, "--mono") == 0 || strcmp(a, "-m") == 0) {
			o->pref = PREF_CELL;
			o->mono = 1;
			i++;
		} else if (strcmp(a, "--blocks") == 0 || strcmp(a, "-b") == 0 ||
			   strcmp(a, "--color") == 0 || strcmp(a, "-c") == 0) {
			o->pref = PREF_CELL;
			i++;
		} else if (strcmp(a, "--fb") == 0 || strcmp(a, "-f") == 0) {
			o->pref = PREF_PIXEL;
			i++;
			if (i < argc && argv[i][0] == '/')
				o->pixel_path = argv[i++];
		} else if ((strcmp(a, "--hold") == 0 || strcmp(a, "-H") == 0) &&
			   i + 1 < argc) {
			o->hold_seconds = atoi(argv[++i]);
			i++;
		} else if ((strcmp(a, "--run-for") == 0 ||
			    strcmp(a, "-t") == 0) && i + 1 < argc) {
			o->run_seconds = atoi(argv[++i]);
			i++;
		} else if ((strcmp(a, "--size") == 0 ||
			    strcmp(a, "-s") == 0) && i + 1 < argc) {
			if (sscanf(argv[++i], "%dx%d", &o->cols, &o->rows) != 2) {
				fprintf(stderr, "bad size `%s', want WxH\n",
				    argv[i]);
				return -1;
			}
			i++;
		} else {
			break;
		}
	}
	return i;
}

/* ── Surface selection ────────────────────────────────────────────── */

static int
cell_open(struct demo_surface *s, const struct demo_opts *o)
{
	/* Seeded, not zeroed: dterm_init overwrites these only when it can
	 * actually determine a size, and a serial console answers neither
	 * TIOCGWINSZ nor $COLUMNS.  Leaving them at zero produced a
	 * zero-extent surface and a division by it. */
	int cols = 78, rows = 39;

	/* dterm reads the window size and what the terminal can do; the
	 * margins are what the header and the closing stats need.  An
	 * explicit size has to be applied after rather than handed in,
	 * since a readable window would otherwise win. */
	dterm_init(&cols, &rows, 2, 5);
	if (o->cols > 0)
		cols = o->cols;
	if (o->rows > 0)
		rows = o->rows;

	s->kind = DEMO_CELL;
	s->width = (unsigned)cols;
	s->height = (unsigned)rows;
	s->aspect_w = 1;
	s->aspect_h = 2;		/* cells are twice as tall as wide */
	s->blocks = !o->mono && dterm_blocks();
	s->pix = NULL;
	return 0;
}

static int
select_surface(struct demo_surface *s, const struct demo_opts *o,
    const struct demo *d)
{
	int supports_framebuffer = d->render_pixel || d->frame_pixel;

	switch (o->pref) {
	case PREF_PIXEL:
		if (!supports_framebuffer) {
			fprintf(stderr, "%s does not support framebuffer "
			    "rendering\n", d->name);
			return -1;
		}
		return pixel_open(s, o->pixel_path, 0);

	case PREF_CELL:
		return cell_open(s, o);

	case PREF_AUTO:
	default:
		/* Probe quietly: no framebuffer here is the ordinary case
		 * on a serial console, not something to report. */
		if (supports_framebuffer &&
		    pixel_open(s, o->pixel_path, 1) == 0)
			return 0;
		return cell_open(s, o);
	}
}


/* ── Hold ─────────────────────────────────────────────────────────── */

/*
 * Hold a finished picture.  Graphics mode ends when the program does,
 * so without this a one-shot render would appear and vanish in the same
 * instant.  A keypress always dismisses; seconds > 0 also gives up on
 * its own, which is what an unattended exhibit needs.
 */
static void
hold(int seconds)
{
	struct timeval tv, *tvp = NULL;
	fd_set rfds;
	unsigned char c;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);
	if (seconds > 0) {
		tv.tv_sec = seconds;
		tv.tv_usec = 0;
		tvp = &tv;
	}
	if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, tvp) > 0)
		(void)read(STDIN_FILENO, &c, 1);
}

/* ── Run ──────────────────────────────────────────────────────────── */

int
demo_main(int argc, char **argv, const struct demo *d)
{
	struct demo_opts o;
	struct demo_surface s;
	int one_shot, argi, status = 0;

	one_shot = (d->render_cell != NULL || d->render_pixel != NULL);
	if (one_shot && (d->frame_cell != NULL || d->frame_pixel != NULL)) {
		fprintf(stderr, "%s: demo is both one-shot and looped\n",
		    d->name);
		return 2;
	}

	memset(&o, 0, sizeof(o));
	memset(&s, 0, sizeof(s));

	argi = parse_opts(argc, argv, &o);
	if (argi < 0) {
		usage(argv[0], d);
		return 2;
	}
	if (d->parse != NULL &&
	    d->parse(argc - argi, argv + argi, d->state) != 0)
		return 2;

	if (select_surface(&s, &o, d) != 0)
		return 1;

	signal(SIGINT, on_interrupt);
	/* Raw input only where input is actually read: the frame loop
	 * polls for a keypress, and a held picture waits for one.  A
	 * one-shot render onto cells does neither. */
	if (!one_shot || s.kind == DEMO_PIXEL)
		raw_input();

	if (one_shot) {
		uint64_t t0 = now_us(), elapsed;

		perf_demo_track();
		if (s.kind == DEMO_PIXEL)
			d->render_pixel(&s, d->state);
		else
			d->render_cell(&s, d->state);
		elapsed = now_us() - t0;
		printf("\nelapsed: %llu.%03llu s\n",
		    (unsigned long long)(elapsed / 1000000ull),
		    (unsigned long long)((elapsed % 1000000ull) / 1000ull));
		fflush(stdout);
		/* Only a picture that dies with the process needs holding.
		 * Cell output is scrollback: it survives the exit, so waiting
		 * for a keypress there is friction with nothing behind it. */
		if (s.kind == DEMO_PIXEL)
			hold(o.hold_seconds);
	} else {
		uint64_t started = now_us(), prev = 0, frame = 0, elapsed;
		demo_frame_fn fn = (s.kind == DEMO_PIXEL) ? d->frame_pixel
							  : d->frame_cell;

		perf_demo_track();
		for (;;) {
			uint64_t now = now_us();
			/* Since the previous frame *started*, so the interval
			 * covers that frame's own cost.  Zero on the first,
			 * which nothing has to special-case unless it wants
			 * to. */
			uint32_t dt = (prev == 0) ? 0 : (uint32_t)(now - prev);

			prev = now;
			fn(&s, dt, frame++, d->state);
			fflush(stdout);

			if (stop_requested || key_pressed())
				break;
			if (o.run_seconds > 0 &&
			    now_us() - started >=
			    (uint64_t)o.run_seconds * 1000000ull)
				break;
		}
		elapsed = now_us() - started;
		printf("\n%llu frames in %llu.%03llu s (%llu.%llu fps)\n",
		    (unsigned long long)frame,
		    (unsigned long long)(elapsed / 1000000ull),
		    (unsigned long long)((elapsed % 1000000ull) / 1000ull),
		    (unsigned long long)(elapsed ? frame * 1000000ull / elapsed : 0),
		    (unsigned long long)(elapsed
			? (frame * 10000000ull / elapsed) % 10 : 0));
	}

	restore_input();
	if (s.kind == DEMO_PIXEL) {
		pixel_close(&s);
	} else if (!one_shot) {
		/* Only a demo that drew with absolute positioning needs the
		 * cursor put back: dterm_end jumps to the row below the
		 * picture, which is where a one-shot render that streamed
		 * newline-terminated lines already left it.  Doing it anyway
		 * moves the cursor for no reason and scrolls when the picture
		 * reached the bottom. */
		dterm_end();
	}
	return status;
}
