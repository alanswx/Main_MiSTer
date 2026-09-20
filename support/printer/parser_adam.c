#include "printer.h"
#include <string.h>

#include "font5x7.h"

// Bi-directional line buffer for Coleco Adam SmartWriter
typedef struct {
    char line[120];
    int column;
    bool backwards;
} AdamBidi;

static AdamBidi bidi;

void parser_adam_init(JobState *job) {
    (void)job;
    memset(bidi.line, ' ', sizeof(bidi.line));
    bidi.column = 0;
    bidi.backwards = false;
    job->canvas.line_spacing = job->canvas.dpi / 6; // 1/6" standard
    job->canvas.cur_color = (RGBColor){0, 0, 0};
}

static void draw_char(Canvas *c, char ch) {
    if (ch < 32 || ch > 126) ch = ' ';
    int idx = ch - 32;
    int scale = (c->dpi >= 144) ? (c->dpi / 72) : 1;

    for (int col = 0; col < 5; col++) {
        uint8_t bits = font5x7_data[idx][col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                int px = c->head_x + (col * scale);
                int py = c->head_y + (row * scale);
                canvas_plot_dot(c, px, py, c->cur_color);
                if (scale > 1) {
                    canvas_plot_dot(c, px + 1, py, c->cur_color);
                    canvas_plot_dot(c, px, py + 1, c->cur_color);
                    canvas_plot_dot(c, px + 1, py + 1, c->cur_color);
                }
            }
        }
    }
    c->head_x += 6 * scale;
}

static void flush_bidi_line(JobState *job) {
    Canvas *c = &job->canvas;
    int margin_left = (int)(0.5f * c->dpi);
    int char_spacing = (c->dpi >= 144) ? (6 * (c->dpi / 72)) : 6;

    c->head_x = margin_left;
    for (int col = 0; col < 120; col++) {
        if (bidi.line[col] != ' ') {
            c->head_x = margin_left + (col * char_spacing);
            draw_char(c, bidi.line[col]);
        }
    }
    memset(bidi.line, ' ', sizeof(bidi.line));
    bidi.column = 0;
    bidi.backwards = false;
}

void parser_adam_byte(JobState *job, uint8_t byte) {
    Canvas *c = &job->canvas;
    int margin_bottom = c->height - (int)(0.5f * c->dpi);

    if (byte == 0x08) { // Backspace
        if (bidi.column > 0) bidi.column--;
    } else if (byte == 0x0D) { // CR
        flush_bidi_line(job);
    } else if (byte == 0x0A) { // LF
        flush_bidi_line(job);
        c->head_y += c->line_spacing;
        if (c->head_y >= margin_bottom) {
            job_commit_page(job);
        }
    } else if (byte == 0x0C) { // FF
        flush_bidi_line(job);
        job_commit_page(job);
    } else if (byte == 0x11) { // Set reverse print direction
        bidi.backwards = true;
    } else if (byte == 0x12) { // Set forward print direction
        bidi.backwards = false;
    } else if (byte >= 32 && byte <= 126) {
        if (bidi.column >= 0 && bidi.column < 120) {
            bidi.line[bidi.column] = (char)byte;
        }
        if (bidi.backwards) {
            if (bidi.column > 0) bidi.column--;
        } else {
            if (bidi.column < 119) bidi.column++;
        }
    }
}
