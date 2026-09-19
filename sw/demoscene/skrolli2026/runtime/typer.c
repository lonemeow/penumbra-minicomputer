/*
 * typer.c — plays a stream from tools/text2typer.py into the cells.
 *
 * Every step is placed on song time, not frame time: a slow frame
 * types several characters at once and a fast one none, and the text
 * reaches each point in the song at the same moment on every run.
 */

#include "rt.h"

#define OP_END          0x00
#define OP_FG           0x01
#define OP_BG           0x02
#define OP_RESET        0x03
#define OP_PAUSE        0x04
#define OP_SPEED        0x05
#define OP_CLEAR        0x06
#define OP_NEWLINE      0x0A

#define DEFAULT_FG      RT_COL_GREY
#define DEFAULT_BG      RT_COL_BLACK

void rt_typer_start(rt_typer_t *t, const unsigned char *text,
                    uint32_t col, uint32_t row,
                    uint32_t cols, uint32_t rows,
                    fix16_t chars_per_row, fix16_t start)
{
    if (col + cols > RT_TEXT_W || row + rows > RT_TEXT_H)
        rt_panic("typer window is off the screen");

    t->p = text;
    t->next = start;
    t->step = fix16_div(FIX16_ONE, chars_per_row);
    t->col0 = t->col = (uint8_t)col;
    t->row0 = t->row = (uint8_t)row;
    t->cols = (uint8_t)cols;
    t->rows = (uint8_t)rows;
    t->fg = DEFAULT_FG;
    t->bg = DEFAULT_BG;
    t->done = 0;
}

int rt_typer_update(rt_typer_t *t, fix16_t now)
{
    volatile uint32_t *cells = rt_cells();

    while (!t->done && t->next <= now) {
        uint8_t op = *t->p++;

        switch (op) {
        case OP_END:
            t->done = 1;
            break;
        case OP_FG:
            t->fg = *t->p++;
            break;
        case OP_BG:
            t->bg = *t->p++;
            break;
        case OP_RESET:
            t->fg = DEFAULT_FG;
            t->bg = DEFAULT_BG;
            break;
        case OP_PAUSE:
            t->next += fix16_from_int(*t->p++);
            break;
        case OP_SPEED:
            t->step = (fix16_t)((uint32_t)t->p[0] | (uint32_t)t->p[1] << 8 |
                                (uint32_t)t->p[2] << 16 | (uint32_t)t->p[3] << 24);
            t->p += 4;
            break;
        case OP_CLEAR:
            for (uint32_t r = t->row0; r < t->row0 + t->rows; r++)
                for (uint32_t c = t->col0; c < t->col0 + t->cols; c++)
                    cells[r * RT_TEXT_W + c] = RT_CELL(' ', t->fg, t->bg);
            t->col = t->col0;
            t->row = t->row0;
            break;
        case OP_NEWLINE:
            t->col = t->col0;
            t->row++;
            t->next += t->step;
            break;
        default:
            if (t->col >= t->col0 + t->cols || t->row >= t->row0 + t->rows)
                rt_panic("typed text runs outside its window");
            cells[t->row * RT_TEXT_W + t->col] = RT_CELL(op, t->fg, t->bg);
            t->col++;
            t->next += t->step;
            break;
        }
    }

    /* After a full line, or a newline below the last row, the next cell
     * is past the window; the cursor waits on its edge instead. */
    uint32_t col = t->col < t->col0 + t->cols ? t->col : t->col0 + t->cols - 1u;
    uint32_t row = t->row < t->row0 + t->rows ? t->row : t->row0 + t->rows - 1u;
    int on_next_cell = col == t->col && row == t->row;

    if (t->done) {
        rt_display_cursor_hide();
    } else {
        /* The cursor inverts its cell, so a black-on-black one hides it;
         * a blank in the current colours shows it in the text's colour.
         * Clamped to the edge, it sits on typed text and leaves it be. */
        if (on_next_cell)
            cells[row * RT_TEXT_W + col] = RT_CELL(' ', t->fg, t->bg);
        rt_display_cursor(col, row);
    }

    return t->done;
}
