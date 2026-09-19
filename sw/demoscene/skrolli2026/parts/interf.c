#include "rt.h"

static void setup(void)
{
}

static void cleanup(void)
{
}

static int precalc(uint32_t deadline_ticks)
{
    (void)deadline_ticks;
    return 1;           /* nothing to compute yet */
}

static void render_frame(fix16_t now, fix16_t start, fix16_t end)
{

}

const rt_part_t part_interf = {
    .name         = "interf",
    .mode         = RT_MODE_FB,
    .frame        = render_frame,
    .precalc_init = setup,
    .precalc_step = precalc,
    .leave        = cleanup
};
