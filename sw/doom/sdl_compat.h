// The SDL primitives that otherwise-portable Chocolate Doom sources call
// as a general-purpose runtime, so those sources build unmodified.
//
// Not a general SDL shim: anything wanting real SDL behaviour — surfaces,
// events, audio devices — gets a NetBSD implementation instead.

#ifndef PENUMBRA_SDL_COMPAT_H
#define PENUMBRA_SDL_COMPAT_H

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/endian.h>

#define SDL_SwapLE16(x) le16toh(x)
#define SDL_SwapLE32(x) le32toh(x)
#define SDL_SwapBE16(x) be16toh(x)
#define SDL_SwapBE32(x) be32toh(x)

// SDL's own magic values; i_swap.h compares SDL_BYTEORDER against them.
#define SDL_LIL_ENDIAN 1234
#define SDL_BIG_ENDIAN 4321
#if _BYTE_ORDER == _BIG_ENDIAN
#define SDL_BYTEORDER SDL_BIG_ENDIAN
#else
#define SDL_BYTEORDER SDL_LIL_ENDIAN
#endif

#define SDL_qsort qsort
#define SDL_free  free

// I_ShutdownGraphics runs from the atexit chain above every call site.
#define SDL_Quit() ((void)0)

#define SDL_MESSAGEBOX_ERROR 0x00000010

// Only ever pointers here, so incomplete types satisfy the declarations
// in i_input.h and txt_sdl.h.
typedef union SDL_Event SDL_Event;
typedef struct SDL_Window SDL_Window;

// i_joystick.h numbers its own buttons on from this, so it must match the
// SDL version upstream builds against (2.0.14, TOUCHPAD = 20).
#define SDL_CONTROLLER_BUTTON_MAX 21

#define PREF_HOME_SUBDIR ".chocodoom"

// Copy dir with a trailing slash — M_StringJoin concatenates without a
// separator — and create it, as SDL_GetPrefPath's contract requires.
// NULL sends m_config.c to its exedir fallback.
static inline char *sdl_pref_dir(const char *dir)
{
    size_t len = strlen(dir);
    int have_slash = (len > 0 && dir[len - 1] == '/');
    char *buf = malloc(len + (have_slash ? 0 : 1) + 1);

    if (buf == NULL)
    {
        return NULL;
    }

    strcpy(buf, dir);
    if (!have_slash)
    {
        strcat(buf, "/");
    }

    if (mkdir(buf, 0755) != 0 && errno != EEXIST)
    {
        fprintf(stderr, "sdl_pref_path: cannot create %s: %s\n",
                buf, strerror(errno));
        free(buf);
        return NULL;
    }

    return buf;
}

static inline char *sdl_pref_path(const char *org, const char *app)
{
    const char *env = getenv("CHOCOLATE_DOOM_PREF_PATH");
    const char *home;
    char *dir, *result;

    (void) org;
    (void) app;

    if (env != NULL)
    {
        return sdl_pref_dir(env);
    }

    home = getenv("HOME");
    if (home == NULL)
    {
        return NULL;
    }

    dir = malloc(strlen(home) + 1 + strlen(PREF_HOME_SUBDIR) + 1);
    if (dir == NULL)
    {
        return NULL;
    }

    strcpy(dir, home);
    strcat(dir, "/");
    strcat(dir, PREF_HOME_SUBDIR);

    result = sdl_pref_dir(dir);
    free(dir);

    return result;
}

// I_Error writes the message to stderr and flushes before reaching here.
static inline void sdl_message_box(unsigned flags, const char *title,
                                   const char *message)
{
    (void) flags;
    (void) title;
    (void) message;
}

#define SDL_GetPrefPath(org, app) sdl_pref_path((org), (app))
#define SDL_ShowSimpleMessageBox(flags, title, message, window) \
    ((void)(window), sdl_message_box((flags), (title), (message)))

#endif /* PENUMBRA_SDL_COMPAT_H */
