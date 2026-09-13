/* audio.c — CLASS_AUDIO bring-up. */

#include "rt_internal.h"
#include "audio.h"

uint32_t rt_audio_base;

void rt_audio_init(uint32_t bootdata)
{
    if (!rt_find_device(bootdata, ACFG_CLASS_AUDIO, &rt_audio_base, 0))
        rt_panic("no CLASS_AUDIO device");

    uint32_t cap = AU_REG(PA_CAP);
    if (PA_CAP_CHANNELS(cap) < AUDIO_TRACKS)
        rt_panic("audio device has fewer than 4 channels");
    if ((cap & (PA_CAP_PITCH | PA_CAP_VOLUME)) !=
        (PA_CAP_PITCH | PA_CAP_VOLUME))
        rt_panic("audio device lacks per-channel rate and gain");
    if (AU_REG(PA_RATE) != AUDIO_RATE)
        rt_panic("audio output rate is not 48 kHz");

    /* The ROM does not reset it: a program run before may have left it playing. */
    AU_REG(PA_CTRL)       = 0;
    AU_REG(PA_CHAN_EN)    = 0;
    AU_REG(PA_IRQ_ENABLE) = 0;
    for (uint32_t c = 0; c < AUDIO_TRACKS; c++) {
        AU_CH(c, PA_C_WM)   = 0;
        AU_CH(c, PA_C_CTRL) = PA_C_CTRL_FLUSH;
    }
    AU_REG(PA_IRQ_STATUS) = ~0u;
    AU_REG(PA_UNDERRUN)   = ~0u;
}
