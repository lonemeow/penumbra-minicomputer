/* monitor.c — the secondary display.
 *
 * The status monitor runs on its own terminal, as a child of the
 * launcher rather than an /etc/ttys entry: init would respawn it within
 * seconds of the launcher stopping it, which would make an exclusive
 * entry impossible.  Owning it here also means an operator who quits
 * the monitor can be handed a shell on the same terminal.
 */

#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "exhibit.h"

/* Two failures in a row without the terminal having been usable means
 * the monitor cannot run here; stop trying rather than forking in a
 * loop on a machine where a fork costs the better part of a second. */
#define MON_MAX_FAILURES	2

static pid_t	mon_pid = -1;		/* -1 when nothing runs there */
static int	mon_is_shell;		/* a shell holds the terminal */
static int	mon_failures;		/* consecutive abnormal exits */

pid_t
monitor_pid(void)
{
	return mon_pid;
}

int
monitor_running(void)
{
	return mon_pid != -1;
}

/*
 * Ask the terminal how big it is.  A serial line carries no window size
 * for TIOCGWINSZ to report, so the terminal is asked directly: park the
 * cursor far past the bottom right, where it clamps to the last cell,
 * then request a cursor position report and read back ESC [ rows ; cols R.
 * Terminals that do not answer leave the conventional 80x24.
 */
static void
probe_size(int fd, int *rows, int *cols)
{
	struct termios saved, raw;
	struct timeval tv;
	char buf[32];
	fd_set rfds;
	size_t n = 0;
	int r, c;

	*rows = 24;
	*cols = 80;
	memset(buf, 0, sizeof(buf));

	if (tcgetattr(fd, &saved) == -1)
		return;
	raw = saved;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(fd, TCSANOW, &raw) == -1)
		return;

	(void)write(fd, "\033[999;999H\033[6n", 14);

	while (n < sizeof(buf) - 1) {
		FD_ZERO(&rfds);
		FD_SET(fd, &rfds);
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		if (select(fd + 1, &rfds, NULL, NULL, &tv) <= 0)
			break;
		if (read(fd, &buf[n], 1) != 1)
			break;
		if (buf[n++] == 'R')
			break;
	}
	buf[n] = '\0';

	(void)tcsetattr(fd, TCSANOW, &saved);

	if (sscanf(buf, "\033[%d;%dR", &r, &c) == 2 && r > 0 && c > 0) {
		*rows = r;
		*cols = c;
	}
}

/* Run cmd with the given terminal as its stdin/stdout/stderr.  The
 * monitor sizes itself from LINES/COLUMNS, which is why the terminal is
 * measured here rather than configured. */
static pid_t
spawn_on_tty(const struct config *cfg, const struct command *cmd)
{
	char rows[16], cols[16];
	pid_t pid;
	int fd, r, c;

	if (cmd->argv[0] == NULL)
		return -1;
	if ((pid = fork()) != 0)
		return pid;			/* parent, or -1 on failure */

	if (setsid() == -1)
		_exit(127);
	if ((fd = open(cfg->monitor_tty, O_RDWR)) == -1)
		_exit(127);
	if (ioctl(fd, TIOCSCTTY, NULL) == -1)
		_exit(127);
	if (dup2(fd, STDIN_FILENO) == -1 || dup2(fd, STDOUT_FILENO) == -1 ||
	    dup2(fd, STDERR_FILENO) == -1)
		_exit(127);
	if (fd > STDERR_FILENO)
		(void)close(fd);

	probe_size(STDOUT_FILENO, &r, &c);
	(void)snprintf(rows, sizeof(rows), "%d", r);
	(void)snprintf(cols, sizeof(cols), "%d", c);
	(void)setenv("LINES", rows, 1);
	(void)setenv("COLUMNS", cols, 1);
	(void)setenv("TERM", "vt100", 1);

	execv(cmd->argv[0], cmd->argv);
	_exit(127);
}

void
monitor_start(const struct config *cfg)
{
	if (mon_pid != -1)
		return;
	mon_pid = spawn_on_tty(cfg, &cfg->monitor);
	mon_is_shell = 0;
}

void
monitor_stop(void)
{
	int status;

	if (mon_pid == -1)
		return;
	(void)kill(mon_pid, SIGTERM);
	(void)waitpid(mon_pid, &status, 0);
	mon_pid = -1;
	mon_is_shell = 0;
	mon_failures = 0;
}

/*
 * Called when the process on the secondary terminal has been reaped.
 *
 * What replaces it depends on why it left, not merely on what it was:
 *
 *   monitor, exited cleanly    the operator quit it to ask for a shell
 *   monitor, crashed or killed  not a request for anything; try again
 *   shell, however it ended     the operator is done; restore the monitor
 *
 * Only a clean exit is read as a request, so a monitor that dies on
 * startup can never hand out a shell, and a shell that exits at once
 * returns to a monitor rather than bouncing back to another shell.
 */
void
monitor_reaped(const struct config *cfg, pid_t pid, int status)
{
	int clean, was_shell;

	if (pid != mon_pid)
		return;

	clean = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	was_shell = mon_is_shell;
	mon_pid = -1;

	if (was_shell) {
		mon_failures = 0;
		mon_pid = spawn_on_tty(cfg, &cfg->monitor);
		mon_is_shell = 0;
		return;
	}

	if (clean) {
		mon_failures = 0;
		mon_pid = spawn_on_tty(cfg, &cfg->shell);
		mon_is_shell = (mon_pid != -1);
		return;
	}

	/* The monitor failed.  Retry a couple of times — the terminal may
	 * simply have been busy — then leave it alone. */
	if (++mon_failures <= MON_MAX_FAILURES) {
		mon_pid = spawn_on_tty(cfg, &cfg->monitor);
		mon_is_shell = 0;
	}
}
