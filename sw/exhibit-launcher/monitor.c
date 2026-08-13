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

/* Run cmd with the given terminal as its stdin/stdout/stderr.  The
 * monitor sizes itself from LINES/COLUMNS, which is why the terminal is
 * measured here rather than configured. */
static pid_t
spawn_on_tty(const struct config *cfg, const struct command *cmd)
{
	char rows[16], cols[16];
	pid_t pid;
	int r, c;

	if (cmd->argv[0] == NULL)
		return -1;
	if ((pid = fork()) != 0)
		return pid;			/* parent, or -1 on failure */

	if (tty_claim(cfg->monitor_tty) == -1)
		_exit(127);

	tty_probe_size(STDOUT_FILENO, &r, &c);

	/* The probe parks the cursor, and the previous program need not have
	 * tidied up; start the next one from a known terminal. */
	(void)write(STDOUT_FILENO, "\033[0m\033[?25h\033[r\033[2J\033[H", 20);

	(void)snprintf(rows, sizeof(rows), "%d", r);
	(void)snprintf(cols, sizeof(cols), "%d", c);
	(void)setenv("LINES", rows, 1);
	(void)setenv("COLUMNS", cols, 1);
	tty_export_term(cfg->monitor_tty +
	    (strncmp(cfg->monitor_tty, "/dev/", 5) == 0 ? 5 : 0));

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
 * What replaces the reaped process depends on why it left, not on what
 * it was.  Only a clean exit counts as a request, which is what stops a
 * monitor that dies on startup from handing out a shell:
 *
 *   monitor, exited cleanly     the operator asked for a shell
 *   monitor, crashed or killed  not a request; try again
 *   shell, however it ended     the operator is done; restore the monitor
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
