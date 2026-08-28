// Entry point.
//
// Upstream's i_main.c wraps SDL_main, but the rest of what it does is not
// SDL's business: m_argv.c frees and replaces myargv, so the vector has to
// be a heap copy, and M_SetExeDir is the only thing that ever assigns
// exedir, which m_config.c compares against unconditionally.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "config.h"
#include "doomtype.h"
#include "i_system.h"
#include "m_argv.h"
#include "m_misc.h"

void D_DoomMain(void);

// Upstream leaves signals to SDL, which this port does not have.  Running
// the exit chain rather than dying outright restores the console, saves
// the config, and reports an interrupted -timedemo's tally so far.
//
// I_Quit is not async-signal-safe; the guard keeps a second signal from
// re-entering it, and makes an interrupt during a long startup phase land
// on _exit instead of appearing to hang.
static volatile sig_atomic_t quitting;

static void
on_terminate(int sig)
{
    (void) sig;

    if (quitting)
    {
        _exit(1);
    }
    quitting = 1;

    I_Quit();
}

int
main(int argc, char **argv)
{
    int i;

    myargc = argc;
    myargv = malloc(argc * sizeof(char *));
    if (myargv == NULL)
    {
        fprintf(stderr, "out of memory saving arguments\n");
        return 1;
    }

    for (i = 0; i < argc; i++)
    {
        myargv[i] = M_StringDuplicate(argv[i]);
    }

    if (M_ParmExists("-version") || M_ParmExists("--version"))
    {
        puts(PACKAGE_STRING);
        return 0;
    }

    M_FindResponseFile();
    M_SetExeDir();

    signal(SIGINT, on_terminate);
    signal(SIGTERM, on_terminate);

    D_DoomMain();

    return 0;
}
