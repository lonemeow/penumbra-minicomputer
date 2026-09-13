/* main.c — device bring-up and the part list. */

#include "runtime/rt.h"

extern const rt_part_t part_testcard;

static const rt_part_t *const parts[] = {
    &part_testcard,
};

void rt_main(uint32_t bootdata);

void rt_main(uint32_t bootdata)
{
    rt_puts("\r\n=== penumbra demo ===\r\n");

    rt_mem_init(bootdata);
    rt_timer_init();
    rt_display_init(bootdata);
    rt_audio_init(bootdata);

    rt_puts("  timer ");
    rt_print_uint(rt_tick_hz());
    rt_puts(" Hz\r\n");

    rt_puts(rt_music_start() ? "  music playing\r\n"
                             : "  no song linked (build with MOD=<file>)\r\n");

    rt_run(parts, (int)(sizeof(parts) / sizeof(parts[0])));

    rt_puts("  done\r\n");
}
