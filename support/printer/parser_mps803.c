#include "printer.h"
#include <string.h>

#include "font5x7.h"

static struct {
    bool esc_mode;
    bool gfx_mode;
    int gfx_cols;
} mps;

void parser_mps803_init(JobState *job) {
    (void)job;
    mps.esc_mode = false;
    mps.gfx_mode = false;
    mps.gfx_cols = 0;
    job->canvas.line_spacing = job->canvas.dpi / 6;
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

// Convert PETSCII to standard printable ASCII
static uint8_t petscii_to_ascii(uint8_t c) {
    if (c >= 0x41 && c <= 0x5A) {
        return c; // Uppercase
    } else if (c >= 0xC1 && c <= 0xDA) {
        return c - 0x60; // Lowercase
    } else if (c >= 0x20 && c <= 0x3F) {
        return c; // Numbers and symbols
    }
    return ' ';
}

void parser_mps803_byte(JobState *job, uint8_t byte) {
    Canvas *c = &job->canvas;
    int margin_left = (int)(0.5f * c->dpi);
    int margin_bottom = c->height - (int)(0.5f * c->dpi);

    if (mps.esc_mode) {
        mps.esc_mode = false;
        if (byte == '8') {
            mps.gfx_mode = true;
        } else if (byte == '7') {
            mps.gfx_mode = false;
        }
        return;
    }

    if (byte == 0x1B) { // ESC
        mps.esc_mode = true;
        return;
    }

    if (byte == 0x08) { // CHR$(8): Bit Image Printing ON
        mps.gfx_mode = true;
        return;
    }

    if (byte == 0x0F) { // CHR$(15): Bit Image Printing OFF
        mps.gfx_mode = false;
        return;
    }

    if (byte == 0x0D) { // Commodore CR acts as CR + LF and cancels bit image
        mps.gfx_mode = false;
        c->head_x = margin_left;
        c->head_y += c->line_spacing;
        if (c->head_y >= margin_bottom) {
            job_commit_page(job);
        }
        return;
    }

    if (byte == 0x0C) { // FF
        job_commit_page(job);
        return;
    }

    if (mps.gfx_mode) {
        // 7-dot graphics on MPS 803 (bits 0..6)
        int v_scale = c->dpi / 72;
        if (v_scale < 1) v_scale = 1;
        for (int pin = 0; pin < 7; pin++) {
            if (byte & (1 << pin)) {
                int py = c->head_y + (pin * v_scale);
                canvas_plot_dot(c, c->head_x, py, c->cur_color);
                if (v_scale > 1) {
                    canvas_plot_dot(c, c->head_x, py + 1, c->cur_color);
                }
            }
        }
        c->head_x += v_scale;
        return;
    }

    // Regular text
    uint8_t ascii = petscii_to_ascii(byte);
    if (ascii >= 32 && ascii <= 126) {
        draw_char(c, (char)ascii);
    }
}
