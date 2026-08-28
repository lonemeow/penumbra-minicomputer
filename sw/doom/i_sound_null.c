// No audio hardware on this machine.
//
// i_sound.c picks a back-end by searching each module's sound_devices
// list for the configured device; a module advertising none is never
// chosen, which is the whole mechanism needed to have no sound.  These
// exist because i_sound.c's module tables name them, and the settings
// because m_config.c binds them and a config file must round-trip.

#include <stddef.h>

#include "config.h"
#include "doomtype.h"
#include "i_sound.h"

const sound_module_t sound_pcsound_module;
const music_module_t music_opl_module;
const music_module_t music_pack_module;

char *snd_dmxoption = "";
char *music_pack_path = "";
char *timidity_cfg_path = "";
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;
int opl_io_port = 0x388;

void I_InitTimidityConfig(void) {}
void I_SetOPLDriverVer(opl_driver_ver_t ver) { (void) ver; }

void
I_OPL_DevMessages(char *msg, size_t sz)
{
    if (sz > 0)
    {
        msg[0] = '\0';
    }
}
