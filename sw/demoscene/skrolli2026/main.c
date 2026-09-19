#include "runtime/rt.h"

extern const rt_part_t part_intro;
extern const rt_part_t part_title;

static const rt_cue_t cues[] = {
//    { RT_POS(0, 0), &part_intro },
    { RT_POS(0, 0), &part_title },
    { RT_POS(40, 0), 0 },
};

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

    rt_run(cues);

    rt_puts("  done\r\n");
}
