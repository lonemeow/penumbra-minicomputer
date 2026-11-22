// Sound effects on the Penumbra audio device, through /dev/pmusic.
//
// The device plays each of its voices from the session's sample memory
// at a rate and gain of its own, and mixes them itself, so an effect
// costs the CPU one event to start it and one for each change of
// position, never a software mix.  An effect is converted into sample
// memory the first time it plays and stays there until the space is
// needed.
//
// Doom keeps snd_channels channels, usually more than the device has
// voices.  A channel starting a sound takes a voice that has fallen
// silent, or else the one playing the least important sound, the way
// S_getChannel chooses among channels.

#include <sys/ioctl.h>

#include <machine/penaudio.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "deh_str.h"
#include "doomtype.h"
#include "i_sound.h"
#include "i_system.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

#define PMUSIC_DEVICE "/dev/pmusic"

// The channel numbers Doom may pass, bounded as i_sdlsound.c bounds them.
#define NUM_CHANNELS 16

// The audio class's ceiling on channels, so the most a device can grant;
// also the width of pst_sounding.
#define MAX_VOICES 32

// An effect in sample memory.
typedef struct cached_sfx_s
{
    int lumpnum;                // linked effects share their lump's copy
    uint32_t offset;            // bytes into sample memory
    uint32_t length;            // bytes, one per frame
    uint32_t rate;              // frames per second it was recorded at
    struct cached_sfx_s *next;
} cached_sfx_t;

typedef struct
{
    cached_sfx_t *sfx;          // last effect started; NULL once evicted
    int channel;                // Doom channel it belongs to, or -1
    int priority;               // that effect's: lower is more important
    unsigned int started;       // start order, to choose among equals
    uint16_t vol_l, vol_r;      // gains last sent
} voice_t;

static int pmusic_fd = -1;
static unsigned int num_voices;
static uint32_t device_rate;
static uint32_t mem_size;

// The most gain a voice is given on either side.  The device sums every
// channel it has, a /dev/audio stream on channel 0 among them, and
// saturates once: with each channel at most an equal share of unity,
// all of them at full scale together still fit.
static uint32_t voice_gain_max;

static voice_t voices[MAX_VOICES];
static int channel_voice[NUM_CHANNELS];     // each channel's voice, or -1
static unsigned int start_count;

static cached_sfx_t *cached_sfx;            // every effect in sample memory
static uint32_t ring_head;                  // where the next effect goes

// The device's state as of the last SUBMIT or GETSTATUS.  SUBMIT reports
// it after applying its events, so a sound started a moment ago already
// counts as sounding.
static struct penaudio_status status;

static boolean use_sfx_prefix;

static boolean
VoiceSounding(int v)
{
    return (status.pst_sounding >> v) & 1;
}

static void
RefreshStatus(void)
{
    if (ioctl(pmusic_fd, PENAUDIO_GETSTATUS, &status) == -1)
    {
        I_Error("%s: GETSTATUS: %s", PMUSIC_DEVICE, strerror(errno));
    }
}

// Every event built here is valid by construction, so a refusal is a bug
// in this file, and stopping beats dropping sounds without a word.
static void
Submit(struct penaudio_event *ev)
{
    struct penaudio_submit ps;

    memset(&ps, 0, sizeof(ps));
    ps.ps_events = ev;
    ps.ps_eventsize = sizeof(*ev);
    ps.ps_nevents = 1;

    // The session's clock never starts, so every event applies during
    // the call and none waits in the queue.
    if (ioctl(pmusic_fd, PENAUDIO_SUBMIT, &ps) == -1)
    {
        I_Error("%s: SUBMIT: %s", PMUSIC_DEVICE, strerror(errno));
    }
    if (ps.ps_nevents != 1)
    {
        I_Error("%s: SUBMIT took no event", PMUSIC_DEVICE);
    }

    status = ps.ps_status;
}

// The pan is linear, as Chocolate Doom's other modules pan: each side
// gets Doom's volume times that side's share of the separation.  The
// largest product, full volume fully to one side, gets voice_gain_max.
#define GAIN_PRODUCT_MAX (127 * 254)

// One side's gain for that product.
static uint16_t
SideGain(int product)
{
    if (product <= 0)
        return 0;
    if (product >= GAIN_PRODUCT_MAX)
        return voice_gain_max;
    // Under 2^15 times at most 0xFFFF: the product fits 32 bits.
    return (uint32_t) product * voice_gain_max / GAIN_PRODUCT_MAX;
}

// Gains for Doom's volume, 0..127 with the menu's effects volume already
// applied, and its separation, 0..254 with 128 centred and lower values
// further left.  0xFFFF is unity on either side.
static void
GainsFor(int vol, int sep, uint16_t *left, uint16_t *right)
{
    *left = SideGain((254 - sep) * vol);
    *right = SideGain(sep * vol);
}

// The 16.16 increment that plays an effect at pitch.  Chocolate Doom's
// law (PitchShift in i_sdlsound.c): pitch p plays NORM_PITCH /
// (2 * NORM_PITCH - p) times as fast, so NORM_PITCH plays as recorded.
static uint32_t
IncFor(const cached_sfx_t *s, int pitch)
{
    uint64_t inc = ((uint64_t) s->rate << 16) / device_rate;
    int denom;

    if (snd_pitchshift && pitch != NORM_PITCH)
    {
        denom = 2 * NORM_PITCH - pitch;
        inc = denom > 0 ? inc * NORM_PITCH / denom : PENAUDIO_INC_MAX;
    }

    // Outside this range the event is refused; a sound at a rate no
    // device plays is better heard at the nearest one.
    if (inc < PENAUDIO_INC_MIN)
    {
        inc = PENAUDIO_INC_MIN;
    }
    if (inc > PENAUDIO_INC_MAX)
    {
        inc = PENAUDIO_INC_MAX;
    }

    return (uint32_t) inc;
}

// ── Sample memory ─────────────────────────────────────────────────

static boolean
Overlaps(const cached_sfx_t *s, uint32_t offset, uint32_t length)
{
    return s->offset < offset + length && offset < s->offset + s->length;
}

// The driver reads an effect from sample memory for as long as a voice
// plays it, so overwriting one that is sounding would be heard.
static boolean
InUse(const cached_sfx_t *s)
{
    unsigned int v;

    for (v = 0; v < num_voices; ++v)
    {
        if (voices[v].sfx == s && VoiceSounding(v))
        {
            return true;
        }
    }

    return false;
}

static void
Evict(cached_sfx_t *s)
{
    cached_sfx_t **p;
    unsigned int v;

    for (p = &cached_sfx; *p != s; p = &(*p)->next)
    {
    }
    *p = s->next;

    for (v = 0; v < num_voices; ++v)
    {
        if (voices[v].sfx == s)
        {
            voices[v].sfx = NULL;
        }
    }

    free(s);
}

// Finds room for length bytes, evicting the effects in the way.  Sample
// memory is used as a ring, so the effects loaded longest ago go first;
// one that is sounding is stepped past rather than overwritten.
static boolean
AllocRegion(uint32_t length, uint32_t *offset)
{
    cached_sfx_t *s, *next, *blocker;
    unsigned int step;

    if (length > mem_size)
    {
        return false;
    }

    RefreshStatus();

    // Each step past a sounding effect leaves it behind the head, and the
    // head wraps at most once, so no more steps than this are needed.
    for (step = 0; step < 2 * num_voices + 2; ++step)
    {
        if (ring_head + length > mem_size)
        {
            ring_head = 0;
        }

        blocker = NULL;
        for (s = cached_sfx; s != NULL; s = s->next)
        {
            if (Overlaps(s, ring_head, length) && InUse(s))
            {
                blocker = s;
                break;
            }
        }

        if (blocker != NULL)
        {
            ring_head = blocker->offset + blocker->length;
            continue;
        }

        for (s = cached_sfx; s != NULL; s = next)
        {
            next = s->next;
            if (Overlaps(s, ring_head, length))
            {
                Evict(s);
            }
        }

        *offset = ring_head;
        ring_head += length;
        return true;
    }

    return false;
}

// The effect's copy in sample memory, loading it on first use; NULL for
// a lump that is not a sound, or one too large to fit beside the effects
// still sounding.
static cached_sfx_t *
CacheSfx(sfxinfo_t *sfxinfo)
{
    cached_sfx_t *s;
    byte *data, *samples;
    unsigned int lumplen;
    uint32_t rate, length, offset, i;
    int8_t *converted;

    for (s = cached_sfx; s != NULL; s = s->next)
    {
        if (s->lumpnum == sfxinfo->lumpnum)
        {
            return s;
        }
    }

    data = W_CacheLumpNum(sfxinfo->lumpnum, PU_STATIC);
    lumplen = W_LumpLength(sfxinfo->lumpnum);

    // DMX's format, validated and trimmed as i_sdlsound.c's CacheSFX
    // does: format 3, a 16-bit rate and a 32-bit length, and samples
    // padded by 16 bytes at each end that DMX does not play.
    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x00)
    {
        W_ReleaseLumpNum(sfxinfo->lumpnum);
        return NULL;
    }
    rate = data[2] | (data[3] << 8);
    length = data[4] | (data[5] << 8) | (data[6] << 16)
           | ((uint32_t) data[7] << 24);
    if (length > lumplen - 8 || length <= 48)
    {
        W_ReleaseLumpNum(sfxinfo->lumpnum);
        return NULL;
    }
    samples = data + 8 + 16;
    length -= 32;

    if (!AllocRegion(length, &offset))
    {
        W_ReleaseLumpNum(sfxinfo->lumpnum);
        return NULL;
    }

    // DMX's samples are unsigned; the device takes signed ones.
    converted = malloc(length);
    s = malloc(sizeof(*s));
    if (converted == NULL || s == NULL)
    {
        I_Error("CacheSfx: out of memory for %u bytes", length);
    }
    for (i = 0; i < length; ++i)
    {
        converted[i] = (int8_t) (samples[i] ^ 0x80);
    }
    if (pwrite(pmusic_fd, converted, length, offset) != (ssize_t) length)
    {
        I_Error("%s: writing a sound: %s", PMUSIC_DEVICE, strerror(errno));
    }
    free(converted);
    W_ReleaseLumpNum(sfxinfo->lumpnum);

    s->lumpnum = sfxinfo->lumpnum;
    s->offset = offset;
    s->length = length;
    s->rate = rate;
    s->next = cached_sfx;
    cached_sfx = s;

    return s;
}

// ── Voices ────────────────────────────────────────────────────────

// The voice for a sound of priority on channel: the channel's own if it
// has one, else a silent one, else the one playing the least important
// sound, the oldest among equals.  -1 when every voice plays something
// more important.
static int
ChooseVoice(int channel, int priority)
{
    int v, best;

    if (channel_voice[channel] >= 0)
    {
        return channel_voice[channel];
    }

    for (v = 0; v < (int) num_voices; ++v)
    {
        if (!VoiceSounding(v))
        {
            return v;
        }
    }

    best = 0;
    for (v = 1; v < (int) num_voices; ++v)
    {
        if (voices[v].priority > voices[best].priority
         || (voices[v].priority == voices[best].priority
          && voices[v].started < voices[best].started))
        {
            best = v;
        }
    }

    return voices[best].priority >= priority ? best : -1;
}

static void
TakeVoice(int v, int channel)
{
    if (voices[v].channel >= 0)
    {
        channel_voice[voices[v].channel] = -1;
    }
    voices[v].channel = channel;
    channel_voice[channel] = v;
}

// ── The sound module ──────────────────────────────────────────────

static boolean
I_Penumbra_InitSound(GameMission_t mission)
{
    struct penaudio_info info;
    uint32_t granted;
    int i;

    use_sfx_prefix = (mission == doom || mission == strife);

    for (i = 0; i < NUM_CHANNELS; ++i)
    {
        channel_voice[i] = -1;
    }
    for (i = 0; i < MAX_VOICES; ++i)
    {
        voices[i].sfx = NULL;
        voices[i].channel = -1;
    }

    pmusic_fd = open(PMUSIC_DEVICE, O_RDWR);
    if (pmusic_fd == -1)
    {
        fprintf(stderr, "I_Penumbra_InitSound: %s: %s\n",
                PMUSIC_DEVICE, strerror(errno));
        return false;
    }

    // Every voice the device can spare.  A /dev/audio stream opened
    // before this holds one, and the grant leaves it out.
    granted = MAX_VOICES;
    if (ioctl(pmusic_fd, PENAUDIO_GETINFO, &info) == -1
     || ioctl(pmusic_fd, PENAUDIO_ALLOC, &granted) == -1
     || ioctl(pmusic_fd, PENAUDIO_SETMEM, &info.pi_maxmem) == -1)
    {
        fprintf(stderr, "I_Penumbra_InitSound: %s: %s\n",
                PMUSIC_DEVICE, strerror(errno));
        close(pmusic_fd);
        pmusic_fd = -1;
        return false;
    }
    if (granted == 0 || info.pi_nvoices == 0 || info.pi_rate == 0
     || info.pi_maxmem == 0)
    {
        fprintf(stderr, "I_Penumbra_InitSound: %s has no voice or "
                "sample memory to spare\n", PMUSIC_DEVICE);
        close(pmusic_fd);
        pmusic_fd = -1;
        return false;
    }

    num_voices = granted;
    voice_gain_max = 0xFFFF / info.pi_nvoices;
    device_rate = info.pi_rate;
    mem_size = info.pi_maxmem;
    ring_head = 0;
    memset(&status, 0, sizeof(status));

    return true;
}

static void
I_Penumbra_ShutdownSound(void)
{
    if (pmusic_fd == -1)
    {
        return;
    }

    // Closing the session silences its voices and releases them and the
    // sample memory.
    close(pmusic_fd);
    pmusic_fd = -1;

    while (cached_sfx != NULL)
    {
        Evict(cached_sfx);
    }
}

static int
I_Penumbra_GetSfxLumpNum(sfxinfo_t *sfx)
{
    char namebuf[9];

    // Linked effects play their link's lump.
    if (sfx->link != NULL)
    {
        sfx = sfx->link;
    }

    // Doom and Strife prefix their sound lumps with "DS"; Heretic and
    // Hexen do not.
    if (use_sfx_prefix)
    {
        M_snprintf(namebuf, sizeof(namebuf), "ds%s", DEH_String(sfx->name));
    }
    else
    {
        M_StringCopy(namebuf, DEH_String(sfx->name), sizeof(namebuf));
    }

    return W_GetNumForName(namebuf);
}

// Called once per frame, before S_UpdateSounds asks which channels are
// still playing.
static void
I_Penumbra_UpdateSound(void)
{
    if (pmusic_fd != -1)
    {
        RefreshStatus();
    }
}

static void
I_Penumbra_UpdateSoundParams(int handle, int vol, int sep)
{
    struct penaudio_event ev;
    uint16_t left, right;
    int v;

    if (pmusic_fd == -1 || handle < 0 || handle >= NUM_CHANNELS
     || (v = channel_voice[handle]) < 0)
    {
        return;
    }

    // Doom recomputes every channel's position each frame; most have not
    // moved, and an unchanged gain is a system call saved.
    GainsFor(vol, sep, &left, &right);
    if (left == voices[v].vol_l && right == voices[v].vol_r)
    {
        return;
    }

    memset(&ev, 0, sizeof(ev));
    ev.pe_voice = v;
    ev.pe_set = PENAUDIO_SET_VOL;
    ev.pe_vol_l = left;
    ev.pe_vol_r = right;
    Submit(&ev);

    voices[v].vol_l = left;
    voices[v].vol_r = right;
}

static int
I_Penumbra_StartSound(sfxinfo_t *sfxinfo, int channel, int vol, int sep,
                      int pitch)
{
    struct penaudio_event ev;
    cached_sfx_t *s;
    int v;

    if (pmusic_fd == -1 || channel < 0 || channel >= NUM_CHANNELS)
    {
        return -1;
    }

    RefreshStatus();
    v = ChooseVoice(channel, sfxinfo->priority);
    if (v < 0)
    {
        return -1;
    }

    s = CacheSfx(sfxinfo);
    if (s == NULL)
    {
        return -1;
    }

    TakeVoice(v, channel);
    voices[v].sfx = s;
    voices[v].priority = sfxinfo->priority;
    voices[v].started = start_count++;
    GainsFor(vol, sep, &voices[v].vol_l, &voices[v].vol_r);

    // The fields of one event apply in the order of their SET_ bits, so
    // the sound starts at its own rate and gain.  No loop region makes
    // it one-shot.
    memset(&ev, 0, sizeof(ev));
    ev.pe_voice = v;
    ev.pe_set = PENAUDIO_SET_INC | PENAUDIO_SET_VOL | PENAUDIO_SET_PLAY;
    ev.pe_inc = IncFor(s, pitch);
    ev.pe_vol_l = voices[v].vol_l;
    ev.pe_vol_r = voices[v].vol_r;
    ev.pe_format = PENAUDIO_FMT_S8;
    ev.pe_play.pr_offset = s->offset;
    ev.pe_play.pr_length = s->length;
    Submit(&ev);

    return channel;
}

static void
I_Penumbra_StopSound(int handle)
{
    struct penaudio_event ev;
    int v;

    if (pmusic_fd == -1 || handle < 0 || handle >= NUM_CHANNELS
     || (v = channel_voice[handle]) < 0)
    {
        return;
    }

    memset(&ev, 0, sizeof(ev));
    ev.pe_voice = v;
    ev.pe_set = PENAUDIO_SET_STOP;
    Submit(&ev);

    voices[v].channel = -1;
    channel_voice[handle] = -1;
}

static boolean
I_Penumbra_SoundIsPlaying(int handle)
{
    int v;

    if (pmusic_fd == -1 || handle < 0 || handle >= NUM_CHANNELS
     || (v = channel_voice[handle]) < 0)
    {
        return false;
    }

    return VoiceSounding(v);
}

// Effects load on first use instead: sample memory may hold fewer of them
// than a game has, and the ones a level plays are what it should hold.
static void
I_Penumbra_PrecacheSounds(sfxinfo_t *sounds, int num_sounds)
{
    (void) sounds;
    (void) num_sounds;
}

static const snddevice_t sound_penumbra_devices[] =
{
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

const sound_module_t sound_penumbra_module =
{
    sound_penumbra_devices,
    arrlen(sound_penumbra_devices),
    I_Penumbra_InitSound,
    I_Penumbra_ShutdownSound,
    I_Penumbra_GetSfxLumpNum,
    I_Penumbra_UpdateSound,
    I_Penumbra_UpdateSoundParams,
    I_Penumbra_StartSound,
    I_Penumbra_StopSound,
    I_Penumbra_SoundIsPlaying,
    I_Penumbra_PrecacheSounds,
};
