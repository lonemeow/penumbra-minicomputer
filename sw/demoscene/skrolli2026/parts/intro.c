#include "rt.h"
#include "intro_text.h"

static const uint32_t heat_to_cell[] = {
    RT_CELL(0x20, RT_COL_RED, RT_COL_BLACK),
    RT_CELL(0xB0, RT_COL_RED, RT_COL_BLACK),
    RT_CELL(0xB1, RT_COL_RED, RT_COL_BLACK),
    RT_CELL(0xB2, RT_COL_RED, RT_COL_BLACK),
    RT_CELL(0xB2, RT_COL_BLACK, RT_COL_RED),
    RT_CELL(0xB1, RT_COL_BLACK, RT_COL_RED),
    RT_CELL(0xB0, RT_COL_BLACK, RT_COL_RED),
    RT_CELL(0xDB, RT_COL_RED, RT_COL_BLACK),
    RT_CELL(0xB0, RT_COL_RED, RT_COL_YELLOW),
    RT_CELL(0xB1, RT_COL_RED, RT_COL_YELLOW),
    RT_CELL(0xB2, RT_COL_RED, RT_COL_YELLOW),
    RT_CELL(0xB2, RT_COL_YELLOW, RT_COL_RED),
    RT_CELL(0xB1, RT_COL_YELLOW, RT_COL_RED),
    RT_CELL(0xB0, RT_COL_YELLOW, RT_COL_RED),
    RT_CELL(0xDB, RT_COL_YELLOW, RT_COL_BLACK),
};

static uint8_t *fire_front;
static uint8_t *fire_back;

static rt_rand_t rand_state;

#define FIRE_W 32
#define FIRE_H 29

static void setup(void)
{
    fire_front = rt_alloc(34 * 30);
    fire_back = rt_alloc(34 * 30);

    memset(fire_front, 0, 34 * 30);
    memset(fire_back, 0, 34 * 30);

    rt_rand_seed(&rand_state, 12765);
}

static void cleanup(void)
{
    rt_free(fire_front);
    rt_free(fire_back);
}

static void render_fire()
{
    for (int x = 1; x < 33; x++) {
        if (rt_rand_frac(&rand_state) < F16(0.2))
            fire_back[29 * 34 + x] = 255;
        else
            fire_back[29 * 34 + x] = 0;
    }

    uint8_t *front_p = fire_front;
    uint8_t *back_p = fire_back;
    uint32_t *screen_p = rt_cells() + RT_TEXT_W - FIRE_W;
    for (int y = 0; y < 29; y++) {
        for (int x = 0; x < 32; x++) {
            uint32_t sum = (uint32_t)back_p[x] + (uint32_t)back_p[x + 1] + (uint32_t)back_p[x + 2];
            sum += (uint32_t)back_p[x + 34] + (uint32_t)back_p[x + 1 + 34] + (uint32_t)back_p[x + 2 + 34];
            uint8_t heat = (uint8_t)(sum / 6);
            front_p[x + 1] = heat;
            heat >>= 3;
            if (heat > 14)
                heat = 14;
            screen_p[x] = heat_to_cell[heat];
        }
        front_p += 34;
        back_p += 34;
        screen_p += RT_TEXT_W;
    }

    uint8_t *tmp = fire_front;
    fire_front = fire_back;
    fire_back = tmp;
}

static fix16_t last_frame = 0;
static rt_typer_t typer;

static void render_frame(fix16_t now, fix16_t start, fix16_t end)
{
    if (last_frame == 0) {
        memset(rt_cells(), 0, RT_TEXT_H * RT_TEXT_W * 4);
        rt_typer_start(&typer, intro_text, 0, 0, 48, 28, F16(1.5), start);
    }

    rt_typer_update(&typer, now);
    if (now - last_frame > F16(0.2)) {
        render_fire();
        last_frame = now;
    }
}

const rt_part_t part_intro = {
    .name  = "intro",
    .mode  = RT_MODE_TEXT,
    .frame = render_frame,
    .enter = setup,
    .leave = cleanup
};
