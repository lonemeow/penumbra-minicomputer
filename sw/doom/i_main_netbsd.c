// Entry point.
//
// Upstream's i_main.c wraps SDL_main, but the rest of what it does is not
// SDL's business: m_argv.c frees and replaces myargv, so the vector has to
// be a heap copy, and M_SetExeDir is the only thing that ever assigns
// exedir, which m_config.c compares against unconditionally.

#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "doomtype.h"
#include "m_argv.h"
#include "m_misc.h"

void D_DoomMain(void);

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

    D_DoomMain();

    return 0;
}
