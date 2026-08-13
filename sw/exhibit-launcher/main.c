/* exhibit-launcher — the primary display of an exhibit machine.
 *
 * Runs from /etc/ttys in place of a getty, so init respawns it if it
 * ever exits.  It owns both screens: a menu on the terminal it was
 * started on, and a status monitor on a second terminal (monitor.c).
 *
 * The menu and the attract loop are one state machine: the demos exit
 * on a keypress themselves, so an attract program is its own interrupt
 * handler.
 */

#include <sys/select.h>
#include <sys/wait.h>

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "exhibit.h"

#define CONFIG_PATH	"/usr/local/etc/exhibit.conf"

/* Logical keys, distinct from any byte value a terminal can send. */
#define KEY_TIMEOUT	(-1)	/* idle expired, nothing was pressed */
#define KEY_NONE	(-2)	/* nothing usable; loop again */
#define KEY_UP		(-3)
#define KEY_DOWN	(-4)
#define KEY_ENTER	(-5)

/* How long to wait for the rest of an escape sequence before deciding
 * the terminal sent a lone Esc.  Long enough for a slow serial line,
 * short enough not to be felt. */
#define ESC_WAIT_MS	50

static struct termios	saved_term;
static int		term_saved;

static void
term_raw(void)
{
	struct termios t;

	if (tcgetattr(STDIN_FILENO, &saved_term) == -1)
		return;
	term_saved = 1;
	t = saved_term;
	t.c_lflag &= ~(ICANON | ECHO);
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	(void)tcsetattr(STDIN_FILENO, TCSANOW, &t);
}

static void
term_restore(void)
{
	if (term_saved)
		(void)tcsetattr(STDIN_FILENO, TCSANOW, &saved_term);
}

/* Reap whatever has exited without blocking, so a monitor that quits
 * while the menu is idle is noticed promptly. */
static void
reap_children(const struct config *cfg)
{
	pid_t pid;
	int status;

	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
		monitor_reaped(cfg, pid, status);
}

/* Block until `want' exits, dispatching any other child that exits
 * meanwhile so the secondary display keeps being managed. */
static void
wait_for(const struct config *cfg, pid_t want)
{
	pid_t pid;
	int status;

	for (;;) {
		pid = waitpid(-1, &status, 0);
		if (pid == -1) {
			if (errno == EINTR)
				continue;
			return;
		}
		if (pid == want)
			return;
		monitor_reaped(cfg, pid, status);
	}
}

/* One byte from the primary terminal, or -1 if nothing arrives within
 * `ms' milliseconds.  A negative ms waits indefinitely. */
static int
read_byte(int ms)
{
	struct timeval tv, *tvp = NULL;
	fd_set rfds;
	unsigned char c;

	FD_ZERO(&rfds);
	FD_SET(STDIN_FILENO, &rfds);
	if (ms >= 0) {
		tv.tv_sec = ms / 1000;
		tv.tv_usec = (ms % 1000) * 1000;
		tvp = &tv;
	}
	if (select(STDIN_FILENO + 1, &rfds, NULL, NULL, tvp) <= 0)
		return -1;
	if (read(STDIN_FILENO, &c, 1) != 1)
		return -1;
	return c;
}

/* Wait up to `secs' for a keypress; 0 waits forever.  Cursor keys arrive
 * as a three-byte sequence, in either the normal (ESC [ A) or
 * application (ESC O A) form, so both are accepted. */
static int
read_key(int secs)
{
	int c;

	c = read_byte(secs > 0 ? secs * 1000 : -1);
	if (c < 0)
		return (secs > 0) ? KEY_TIMEOUT : KEY_NONE;
	if (c == '\r' || c == '\n')
		return KEY_ENTER;
	if (c != 0x1b)
		return c;

	c = read_byte(ESC_WAIT_MS);
	if (c != '[' && c != 'O')
		return KEY_NONE;		/* a lone Esc, or something else */
	switch (read_byte(ESC_WAIT_MS)) {
	case 'A':
		return KEY_UP;
	case 'B':
		return KEY_DOWN;
	default:
		return KEY_NONE;
	}
}

/* Programs are expected to restore any display mode they set; this
 * covers only terminal state, so one that died first cannot leave the
 * menu unreadable. */
static void
term_reset_display(void)
{
	printf("\033[0m\033[?25h");
}

/* Screen row of the first menu entry, counting the title and the blank
 * line under it.  Rows are 1-based, as the terminal counts them. */
#define MENU_ROW0	3

/* Black on white rather than reverse video, which the display adapter
 * does not implement. */
#define SEL_ON		"\033[30;47m"
#define SEL_OFF		"\033[0m"

/* Only the two changed rows are sent; a full repaint clears the screen
 * and flickers.  ESC [ K removes the old highlight's padding. */
static void
draw_entry(const struct config *cfg, int i, int sel)
{
	printf("\033[%d;1H", MENU_ROW0 + i);
	if (i == sel)
		printf("  " SEL_ON " %s " SEL_OFF "\033[K", cfg->entries[i].label);
	else
		printf("   %s\033[K", cfg->entries[i].label);
	fflush(stdout);
}

static void
draw_menu(const struct config *cfg, int sel)
{
	int i;

	term_reset_display();
	printf("\033[2J\033[H");
	printf("=== Penumbra ===\r\n\r\n");
	for (i = 0; i < cfg->nentries; i++) {
		if (i == sel)
			printf("  " SEL_ON " %s " SEL_OFF "\r\n",
			    cfg->entries[i].label);
		else
			printf("   %s\r\n", cfg->entries[i].label);
	}
	printf("\r\n  Up and Down to choose, Enter to start.\r\n");
	printf("\033[?25l");			/* the menu has no caret */
	fflush(stdout);
}

/* Move the selection, repainting only the rows that changed. */
static int
move_selection(const struct config *cfg, int sel, int delta)
{
	int prev = sel;

	sel = (sel + cfg->nentries + delta) % cfg->nentries;
	if (sel != prev) {
		draw_entry(cfg, prev, sel);
		draw_entry(cfg, sel, sel);
	}
	return sel;
}

/* Hand the primary display to a child and wait for it.  The terminal is
 * returned to its normal mode first: the programs we launch expect a
 * cooked terminal and set up whatever they need themselves. */
static void
run_command(const struct config *cfg, const struct command *cmd, int exclusive)
{
	pid_t pid;

	term_restore();
	if (exclusive)
		monitor_stop();

	printf("\033[?25h\033[2J\033[H");	/* the caret is theirs again */
	fflush(stdout);

	if ((pid = fork()) == 0) {
		execv(cmd->argv[0], cmd->argv);
		_exit(127);
	}
	if (pid != -1)
		wait_for(cfg, pid);

	if (exclusive)
		monitor_start(cfg);
	term_raw();
}

/* Leave a finished picture up briefly.  Nothing is printed over it: the
 * demos size themselves to the screen, so a prompt would scroll the top
 * of the picture away to say something a keypress already implies. */
static void
hold_result(const struct config *cfg)
{
	(void)read_key(cfg->hold_seconds);
}

static void
launch_entry(const struct config *cfg, const struct entry *e)
{
	run_command(cfg, &e->cmd, e->exclusive);
	if (e->pause_after)
		hold_result(cfg);
}

int
main(int argc, char **argv)
{
	const char *path = CONFIG_PATH;
	struct config cfg;
	int attract_next = 0, sel = 0, c;

	/* init appends the terminal name, as it does for getty, so the
	 * positional argument is not ours to interpret. */
	while ((c = getopt(argc, argv, "f:")) != -1) {
		switch (c) {
		case 'f':
			path = optarg;
			break;
		default:
			fprintf(stderr, "usage: %s [-f config] [tty]\n",
			    argv[0]);
			return 1;
		}
	}

	/* init leaves opening the terminal to the program, as getty does.
	 * Claimed before anything is printed, so errors land on it too. */
	if (optind < argc) {
		char dev[64];

		(void)snprintf(dev, sizeof(dev), "/dev/%s", argv[optind]);
		if (tty_claim(dev) == -1) {
			fprintf(stderr, "%s: cannot claim terminal\n", dev);
			return 1;
		}
		tty_export_term(argv[optind]);
	}

	if (config_load(path, &cfg) != 0)
		return 1;

	/* A hangup on the primary terminal must not take the launcher down
	 * while a child still holds the secondary one. */
	(void)signal(SIGHUP, SIG_IGN);
	(void)signal(SIGINT, SIG_IGN);
	(void)signal(SIGQUIT, SIG_IGN);

	monitor_start(&cfg);
	term_raw();

	/* The menu is repainted in full only when something else has had
	 * the screen; moving the selection touches two rows. */
	for (int redraw = 1;;) {
		int key;

		reap_children(&cfg);
		if (redraw) {
			draw_menu(&cfg, sel);
			redraw = 0;
		}

		key = read_key(cfg.idle_seconds);

		switch (key) {
		case KEY_TIMEOUT:
			if (cfg.nattract == 0)
				break;
			run_command(&cfg, &cfg.attract[attract_next], 0);
			attract_next = (attract_next + 1) % cfg.nattract;
			redraw = 1;
			break;
		case KEY_UP:
			sel = move_selection(&cfg, sel, -1);
			break;
		case KEY_DOWN:
			sel = move_selection(&cfg, sel, 1);
			break;
		case KEY_ENTER:
			launch_entry(&cfg, &cfg.entries[sel]);
			redraw = 1;
			break;
		default:
			/* Number keys stay as a shortcut for anyone who
			 * prefers them. */
			if (key >= '1' && key < '1' + cfg.nentries) {
				sel = key - '1';
				launch_entry(&cfg, &cfg.entries[sel]);
				redraw = 1;
			}
			break;
		}
	}
	/* NOTREACHED */
}
