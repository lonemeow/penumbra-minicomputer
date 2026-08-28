// Doom's video output on a NetBSD wscons framebuffer.
//
// Plain MI wscons — GET_FBINFO, MODE_DUMBFB, mmap, PUTCMAP — so this runs
// on any NetBSD display offering an 8bpp indexed framebuffer.

#include <sys/ioctl.h>
#include <sys/mman.h>

#include <dev/wscons/wsconsio.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "doomtype.h"
#include "i_input_netbsd.h"
#include "i_system.h"
#include "i_video.h"
#include "m_config.h"
#include "tables.h"
#include "v_video.h"
#include "z_zone.h"

#define FB_DEVICE "/dev/ttyE0"

pixel_t *I_VideoBuffer = NULL;

boolean screenvisible = true;
boolean screensaver_mode = false;
int vanilla_keyboard_mapping = 1;
int usegamma = 0;
int screen_width = SCREENWIDTH;
int screen_height = SCREENHEIGHT;
int fullscreen = 1;
int aspect_ratio_correct = 1;
int integer_scaling = 0;
int smooth_pixel_scaling = 1;
int vga_porch_flash = 0;
int force_software_renderer = 0;
int png_screenshots = 0;
int usemouse = 0;
unsigned int joywait = 0;
char *video_driver = "";
char *window_position = "";

static int fb_fd = -1;
static int fb_own_fd = 0;
static int fb_mode_set = 0;
static uint8_t *fb_pixels = NULL;
static size_t fb_size = 0;
static unsigned fb_width, fb_height, fb_stride;

// The device renders 8-bit indices; PUTCMAP carries the colours.
static uint8_t cmap_r[256], cmap_g[256], cmap_b[256];

static void
fb_put_cmap(void)
{
    struct wsdisplay_cmap cm;

    cm.index = 0;
    cm.count = 256;
    cm.red = cmap_r;
    cm.green = cmap_g;
    cm.blue = cmap_b;

    if (ioctl(fb_fd, WSDISPLAYIO_PUTCMAP, &cm) != 0)
    {
        fprintf(stderr, "PUTCMAP: %s\n", strerror(errno));
    }
}

// Try stdout's own device first, so running from the console needs no
// argument; fall back to the first virtual terminal.
static int
fb_open(void)
{
    struct wsdisplayio_fbinfo fbi;
    u_int mode;

    fb_fd = fileno(stdout);
    if (ioctl(fb_fd, WSDISPLAYIO_GET_FBINFO, &fbi) != 0)
    {
        fb_fd = open(FB_DEVICE, O_RDWR | O_NOCTTY);
        if (fb_fd < 0)
        {
            I_Error("no framebuffer: %s: %s", FB_DEVICE, strerror(errno));
        }
        fb_own_fd = 1;

        if (ioctl(fb_fd, WSDISPLAYIO_GET_FBINFO, &fbi) != 0)
        {
            I_Error("%s has no framebuffer: %s", FB_DEVICE, strerror(errno));
        }
    }

    if (fbi.fbi_bitsperpixel != 8 || fbi.fbi_pixeltype != WSFB_CI)
    {
        I_Error("framebuffer is %u bpp pixeltype %u; Doom needs 8bpp indexed",
                fbi.fbi_bitsperpixel, fbi.fbi_pixeltype);
    }
    if (fbi.fbi_width < SCREENWIDTH || fbi.fbi_height < SCREENHEIGHT)
    {
        I_Error("framebuffer is %ux%u; Doom needs at least %dx%d",
                fbi.fbi_width, fbi.fbi_height, SCREENWIDTH, SCREENHEIGHT);
    }

    mode = WSDISPLAYIO_MODE_DUMBFB;
    if (ioctl(fb_fd, WSDISPLAYIO_SMODE, &mode) != 0)
    {
        I_Error("SMODE DUMBFB: %s", strerror(errno));
    }
    fb_mode_set = 1;

    fb_size = (size_t) fbi.fbi_fbsize;
    fb_pixels = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fb_fd, 0);
    if (fb_pixels == MAP_FAILED)
    {
        fb_pixels = NULL;
        I_Error("framebuffer mmap: %s", strerror(errno));
    }

    fb_width = fbi.fbi_width;
    fb_height = fbi.fbi_height;
    fb_stride = fbi.fbi_stride;

    return 0;
}

void
I_InitGraphics(void)
{
    fb_open();

    // Both the pixels and the palette are undefined at power-up, so black
    // out the colours before showing anything.
    memset(cmap_r, 0, sizeof(cmap_r));
    memset(cmap_g, 0, sizeof(cmap_g));
    memset(cmap_b, 0, sizeof(cmap_b));
    fb_put_cmap();
    memset(fb_pixels, 0, fb_size);

    // Doom renders here, in cached memory; I_FinishUpdate moves it to the
    // uncached device aperture once per frame.
    I_VideoBuffer = Z_Malloc(SCREENWIDTH * SCREENHEIGHT, PU_STATIC, NULL);
    V_RestoreBuffer();

    I_InputInit(fb_fd);

    // Nothing else restores the console: without this the display stays
    // in DUMBFB and the keyboard in raw mode after the game exits.
    I_AtExit(I_ShutdownGraphics, true);

    screenvisible = true;
}

void
I_ShutdownGraphics(void)
{
    u_int mode = WSDISPLAYIO_MODE_EMUL;

    I_InputShutdown();

    if (fb_pixels != NULL)
    {
        munmap(fb_pixels, fb_size);
        fb_pixels = NULL;
    }
    if (fb_mode_set)
    {
        ioctl(fb_fd, WSDISPLAYIO_SMODE, &mode);
        fb_mode_set = 0;
    }
    if (fb_own_fd)
    {
        close(fb_fd);
        fb_own_fd = 0;
    }
    fb_fd = -1;
}

// The low two bits go because vanilla ran on a 6-bit VGA DAC, and the
// colours were chosen for how they looked there.
void
I_SetPalette(byte *doompalette)
{
    int i;

    for (i = 0; i < 256; ++i)
    {
        cmap_r[i] = gammatable[usegamma][*doompalette++] & ~3;
        cmap_g[i] = gammatable[usegamma][*doompalette++] & ~3;
        cmap_b[i] = gammatable[usegamma][*doompalette++] & ~3;
    }

    fb_put_cmap();
}

int
I_GetPaletteIndex(int r, int g, int b)
{
    int best = 0, best_diff = INT_MAX, diff, i;

    for (i = 0; i < 256; ++i)
    {
        diff = (r - cmap_r[i]) * (r - cmap_r[i])
             + (g - cmap_g[i]) * (g - cmap_g[i])
             + (b - cmap_b[i]) * (b - cmap_b[i]);

        if (diff < best_diff)
        {
            best = i;
            best_diff = diff;
            if (diff == 0)
            {
                break;
            }
        }
    }

    return best;
}

void
I_FinishUpdate(void)
{
    if (fb_pixels == NULL)
    {
        return;
    }

    // One copy only when the destination pitch is Doom's row width; a
    // wider framebuffer, which I_InitGraphics accepts, needs row strides.
    if (fb_stride == SCREENWIDTH)
    {
        memcpy(fb_pixels, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
    }
    else
    {
        pixel_t *src = I_VideoBuffer;
        uint8_t *dst = fb_pixels;
        int y;

        for (y = 0; y < SCREENHEIGHT; ++y)
        {
            memcpy(dst, src, SCREENWIDTH);
            src += SCREENWIDTH;
            dst += fb_stride;
        }
    }
}

void
I_ReadScreen(pixel_t *scr)
{
    memcpy(scr, I_VideoBuffer, SCREENWIDTH * SCREENHEIGHT);
}

// The exit screen is ANSI art meant for a text console; the display is in
// graphics mode by the time this runs.
void I_Endoom(byte *endoom_data) { (void) endoom_data; }

void I_UpdateNoBlit(void) {}
void I_BeginRead(void) {}
void I_StartFrame(void) {}
void I_GraphicsCheckCommandLine(void) {}
void I_CheckIsScreensaver(void) {}
void I_InitWindowTitle(void) {}
void I_InitWindowIcon(void) {}
void I_EnableLoadingDisk(int xoffs, int yoffs) { (void) xoffs; (void) yoffs; }
void I_SetWindowTitle(const char *title) { (void) title; }
void I_DisplayFPSDots(boolean dots_on) { (void) dots_on; }
void I_SetGrabMouseCallback(grabmouse_callback_t func) { (void) func; }

void
I_RegisterWindowIcon(const unsigned int *icon, int width, int height)
{
    (void) icon;
    (void) width;
    (void) height;
}

void
I_BindVideoVariables(void)
{
    M_BindIntVariable("usegamma", &usegamma);
}
