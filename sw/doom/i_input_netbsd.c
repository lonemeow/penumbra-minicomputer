// Doom's keyboard on a wscons keyboard in raw mode.
//
// WSKBD_RAW delivers XT scancodes with the release bit set on key-up,
// which is what Doom wants and what a terminal cannot give: a tty reports
// characters, never releases, so held movement keys would be impossible.
//
// Doom's own key constants are XT scancodes offset by 0x80 — vanilla read
// the PC keyboard directly — so most of the table below is identity.

#include <sys/ioctl.h>

#include <dev/wscons/wsconsio.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "d_event.h"
#include "doomkeys.h"
#include "doomtype.h"
#include "i_input_netbsd.h"
#include "m_config.h"

float mouse_acceleration = 2.0;
int mouse_threshold = 10;

#define SC_RELEASE 0x80
#define SC_EXTENDED 0xE0
#define SC_LSHIFT 0x2A
#define SC_RSHIFT 0x36

static int kbd_fd = -1;
static int kbd_mode_set = 0;
static int shift_held = 0;

static const unsigned char scancode_to_key[128] = {
    [0x01] = KEY_ESCAPE,
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0A] = '9', [0x0B] = '0',
    [0x0C] = KEY_MINUS, [0x0D] = KEY_EQUALS, [0x0E] = KEY_BACKSPACE,
    [0x0F] = KEY_TAB,
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1A] = '[', [0x1B] = ']', [0x1C] = KEY_ENTER, [0x1D] = KEY_RCTRL,
    [0x1E] = 'a', [0x1F] = 's', [0x20] = 'd', [0x21] = 'f', [0x22] = 'g',
    [0x23] = 'h', [0x24] = 'j', [0x25] = 'k', [0x26] = 'l', [0x27] = ';',
    [0x28] = '\'', [0x29] = '`', [0x2A] = KEY_RSHIFT, [0x2B] = '\\',
    [0x2C] = 'z', [0x2D] = 'x', [0x2E] = 'c', [0x2F] = 'v', [0x30] = 'b',
    [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.', [0x35] = '/',
    [0x36] = KEY_RSHIFT, [0x37] = '*', [0x38] = KEY_RALT, [0x39] = ' ',
    [0x3A] = KEY_CAPSLOCK,
    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,
    [0x45] = KEY_NUMLOCK, [0x46] = KEY_SCRLCK,
    [0x47] = KEY_HOME, [0x48] = KEY_UPARROW, [0x49] = KEY_PGUP,
    [0x4B] = KEY_LEFTARROW, [0x4D] = KEY_RIGHTARROW,
    [0x4F] = KEY_END, [0x50] = KEY_DOWNARROW, [0x51] = KEY_PGDN,
    [0x52] = KEY_INS, [0x53] = KEY_DEL,
    [0x57] = KEY_F11, [0x58] = KEY_F12,
};

static const char shifted_digits[] = ")!@#$%^&*(";

// data2 carries the character the key would type, which the menu uses for
// save-game names; keys with no printable form report zero.
static int
key_to_char(int key)
{
    if (key >= 'a' && key <= 'z')
    {
        return shift_held ? key - 'a' + 'A' : key;
    }
    if (key >= '0' && key <= '9')
    {
        return shift_held ? shifted_digits[key - '0'] : key;
    }
    if (key > 0x20 && key < 0x7F)
    {
        return key;
    }

    return 0;
}

void
I_InputInit(int fd)
{
    int mode = WSKBD_RAW;
    int flags;

    kbd_fd = fd;

    flags = fcntl(kbd_fd, F_GETFL, 0);
    if (flags < 0 || fcntl(kbd_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        fprintf(stderr, "keyboard O_NONBLOCK: %s; keys will not work\n",
                strerror(errno));
        kbd_fd = -1;
        return;
    }

    if (ioctl(kbd_fd, WSKBDIO_SETMODE, &mode) != 0)
    {
        fprintf(stderr, "keyboard raw mode: %s; keys will not work\n",
                strerror(errno));
        kbd_fd = -1;
        return;
    }

    kbd_mode_set = 1;
}

void
I_InputShutdown(void)
{
    int mode = WSKBD_TRANSLATED;

    if (kbd_mode_set)
    {
        ioctl(kbd_fd, WSKBDIO_SETMODE, &mode);
        kbd_mode_set = 0;
    }

    kbd_fd = -1;
}

void
I_StartTic(void)
{
    unsigned char buf[64];
    ssize_t n, i;
    event_t ev;

    if (kbd_fd < 0)
    {
        return;
    }

    while ((n = read(kbd_fd, buf, sizeof(buf))) > 0)
    {
        for (i = 0; i < n; ++i)
        {
            unsigned char sc = buf[i];
            int released, key;

            // The prefix only widens the code space; every extended key
            // this table knows shares its base scancode's meaning.
            if (sc == SC_EXTENDED)
            {
                continue;
            }

            released = (sc & SC_RELEASE) != 0;
            sc &= ~SC_RELEASE;

            if (sc == SC_LSHIFT || sc == SC_RSHIFT)
            {
                shift_held = !released;
            }

            key = scancode_to_key[sc];
            if (key == 0)
            {
                continue;
            }

            ev.type = released ? ev_keyup : ev_keydown;
            ev.data1 = key;
            ev.data2 = released ? 0 : key_to_char(key);
            ev.data3 = 0;
            D_PostEvent(&ev);
        }

        if ((size_t) n < sizeof(buf))
        {
            break;
        }
    }
}

void I_ReadMouse(void) {}
void I_StartTextInput(int x1, int y1, int x2, int y2)
{
    (void) x1; (void) y1; (void) x2; (void) y2;
}
void I_StopTextInput(void) {}

void
I_BindInputVariables(void)
{
    M_BindIntVariable("mouse_threshold", &mouse_threshold);
}
