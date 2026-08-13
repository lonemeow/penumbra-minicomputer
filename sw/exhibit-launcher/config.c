/* config.c — the exhibit configuration file.
 *
 * The menu is data rather than code so that the same binary serves
 * different audiences: swapping demos, or replacing one with something
 * better, is an edit on the running machine.
 *
 * Directives, one per line, '#' comments to end of line:
 *
 *   monitor    <device> <command...>
 *   shell      <command...>
 *   idle       <seconds>              0 disables the attract loop
 *   hold       <seconds>              how long a finished picture stays
 *   attract    <command...>           cycled while idle, in order
 *   entry      <flags> "<label>" <command...>
 *
 * Entry flags are a word of letters, or '-' for none:
 *
 *   x   needs the machine to itself; the monitor is stopped while it
 *       runs and started again afterwards
 *   p   draws once and exits, so the result is held on screen until a
 *       visitor presses a key or `hold' seconds pass
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "exhibit.h"

/* Pull the next whitespace-delimited word out of *sp, advancing it.
 * A word may be "quoted" to keep spaces, which is how labels are
 * written.  Returns NULL when the line is exhausted. */
static char *
next_word(char **sp)
{
	char *s = *sp, *start;

	while (*s == ' ' || *s == '\t')
		s++;
	if (*s == '\0' || *s == '#')
		return NULL;

	if (*s == '"') {
		start = ++s;
		while (*s != '\0' && *s != '"')
			s++;
	} else {
		start = s;
		while (*s != '\0' && *s != ' ' && *s != '\t')
			s++;
	}
	if (*s != '\0')
		*s++ = '\0';
	*sp = s;
	return start;
}

/* Fill cmd from the remaining words on the line.  The words point into
 * the caller's line buffer, which is reused, so each is copied out. */
static int
parse_command(char **sp, struct command *cmd, const char *path, int lineno)
{
	int n = 0;
	char *w;

	while ((w = next_word(sp)) != NULL) {
		if (n >= EX_MAX_ARGS) {
			fprintf(stderr, "%s:%d: too many arguments\n",
			    path, lineno);
			return -1;
		}
		cmd->argv[n++] = strdup(w);
	}
	cmd->argv[n] = NULL;
	if (n == 0) {
		fprintf(stderr, "%s:%d: missing command\n", path, lineno);
		return -1;
	}
	return 0;
}

int
config_load(const char *path, struct config *cfg)
{
	FILE *fp;
	char buf[512];
	int lineno = 0, bad = 0;

	memset(cfg, 0, sizeof(*cfg));

	if ((fp = fopen(path, "r")) == NULL) {
		fprintf(stderr, "%s: cannot open\n", path);
		return -1;
	}

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		char *s = buf, *kw, *w;

		lineno++;
		buf[strcspn(buf, "\n")] = '\0';

		if ((kw = next_word(&s)) == NULL)
			continue;			/* blank or comment */

		if (strcmp(kw, "monitor") == 0) {
			if ((w = next_word(&s)) == NULL)
				goto syntax;
			cfg->monitor_tty = strdup(w);
			if (parse_command(&s, &cfg->monitor, path, lineno) != 0)
				bad = 1;
		} else if (strcmp(kw, "shell") == 0) {
			if (parse_command(&s, &cfg->shell, path, lineno) != 0)
				bad = 1;
		} else if (strcmp(kw, "hold") == 0) {
			if ((w = next_word(&s)) == NULL)
				goto syntax;
			cfg->hold_seconds = atoi(w);
		} else if (strcmp(kw, "idle") == 0) {
			if ((w = next_word(&s)) == NULL)
				goto syntax;
			cfg->idle_seconds = atoi(w);
		} else if (strcmp(kw, "attract") == 0) {
			if (cfg->nattract >= EX_MAX_ATTRACT) {
				fprintf(stderr, "%s:%d: too many attract "
				    "programs\n", path, lineno);
				bad = 1;
				continue;
			}
			if (parse_command(&s, &cfg->attract[cfg->nattract],
			    path, lineno) != 0)
				bad = 1;
			else
				cfg->nattract++;
		} else if (strcmp(kw, "entry") == 0) {
			struct entry *e;

			if (cfg->nentries >= EX_MAX_ENTRIES) {
				fprintf(stderr, "%s:%d: too many entries\n",
				    path, lineno);
				bad = 1;
				continue;
			}
			e = &cfg->entries[cfg->nentries];
			if ((w = next_word(&s)) == NULL)
				goto syntax;
			for (; *w != '\0'; w++) {
				switch (*w) {
				case '-':
					break;
				case 'x':
					e->exclusive = 1;
					break;
				case 'p':
					e->pause_after = 1;
					break;
				default:
					fprintf(stderr, "%s:%d: unknown entry "
					    "flag `%c'\n", path, lineno, *w);
					bad = 1;
					break;
				}
			}
			if ((w = next_word(&s)) == NULL)
				goto syntax;
			e->label = strdup(w);
			if (parse_command(&s, &e->cmd, path, lineno) != 0)
				bad = 1;
			else
				cfg->nentries++;
		} else {
			fprintf(stderr, "%s:%d: unknown directive `%s'\n",
			    path, lineno, kw);
			bad = 1;
		}
		continue;
 syntax:
		fprintf(stderr, "%s:%d: malformed `%s'\n", path, lineno, kw);
		bad = 1;
	}
	fclose(fp);

	if (cfg->hold_seconds == 0)
		cfg->hold_seconds = 15;

	if (cfg->nentries == 0) {
		fprintf(stderr, "%s: no menu entries\n", path);
		bad = 1;
	}
	return bad ? -1 : 0;
}
