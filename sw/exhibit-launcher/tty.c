/* tty.c — taking possession of a terminal.
 *
 * init only forks and execs a program named in /etc/ttys; opening the
 * terminal and claiming it is left to the program, as getty does.  Used
 * for the launcher's own terminal and for the monitor's.
 */

#include <sys/ioctl.h>
#include <sys/select.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <ttyent.h>
#include <unistd.h>

#include "exhibit.h"

/*
 * Make dev this process's controlling terminal and its standard streams.
 * Returns 0, or -1 with the process left as it was found.
 */
int
tty_claim(const char *dev)
{
	int fd;

	/* EPERM means already a session leader, which is where we need to be. */
	if (setsid() == -1 && errno != EPERM)
		return -1;

	if ((fd = open(dev, O_RDWR)) == -1)
		return -1;
	if (ioctl(fd, TIOCSCTTY, NULL) == -1) {
		(void)close(fd);
		return -1;
	}
	if (dup2(fd, STDIN_FILENO) == -1 || dup2(fd, STDOUT_FILENO) == -1 ||
	    dup2(fd, STDERR_FILENO) == -1) {
		(void)close(fd);
		return -1;
	}
	if (fd > STDERR_FILENO)
		(void)close(fd);
	return 0;
}

/*
 * A serial line carries no window size for TIOCGWINSZ, so ask the
 * terminal: park the cursor past the bottom right, where it clamps, and
 * read the cursor position report.  Silent terminals leave 80x24.
 */
void
tty_probe_size(int fd, int *rows, int *cols)
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

/*
 * Export TERM from the terminal's type in /etc/ttys, as getty does.
 * `name' carries no /dev/ prefix.
 */
void
tty_export_term(const char *name)
{
	struct ttyent *tp;

	if ((tp = getttynam(name)) != NULL && tp->ty_type != NULL)
		(void)setenv("TERM", tp->ty_type, 1);
	endttyent();
}
