/* exhibit.h — shared types for the exhibit launcher. */

#ifndef EXHIBIT_H
#define EXHIBIT_H

#include <sys/types.h>

#define EX_MAX_ENTRIES	16	/* menu entries on the primary display */
#define EX_MAX_ATTRACT	16	/* programs cycled while idle */
#define EX_MAX_ARGS	8	/* argv slots per command, excluding NULL */

/* A command line, already split, NULL-terminated for execv(). */
struct command {
	char	*argv[EX_MAX_ARGS + 1];
};

struct entry {
	char		*label;		/* shown in the menu */
	struct command	 cmd;
	int		 exclusive;	/* stop the monitor while it runs */
	int		 pause_after;	/* hold the result on screen when it
					 * exits, for programs that draw once
					 * and return immediately */
};

struct config {
	/* Secondary display: a status monitor on its own terminal, whose
	 * size is measured at spawn time rather than configured. */
	char		*monitor_tty;
	struct command	 monitor;
	struct command	 shell;		/* replaces the monitor when it exits */

	int		 idle_seconds;	/* 0 disables the attract loop */

	struct entry	 entries[EX_MAX_ENTRIES];
	int		 nentries;
	struct command	 attract[EX_MAX_ATTRACT];
	int		 nattract;
};

/* tty.c */
int	tty_claim(const char *dev);
void	tty_probe_size(int fd, int *rows, int *cols);
void	tty_export_term(const char *name);

/* config.c */
int	config_load(const char *path, struct config *cfg);

/* monitor.c — the secondary display, supervised as a child of the
 * launcher so that it can be taken down for exclusive entries. */
void	monitor_start(const struct config *cfg);
void	monitor_stop(void);
int	monitor_running(void);
void	monitor_reaped(const struct config *cfg, pid_t pid, int status);
pid_t	monitor_pid(void);

#endif /* EXHIBIT_H */
