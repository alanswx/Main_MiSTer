#include "printer.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const RGBColor IW_COLORS[8] = {
    {0, 0, 0},        // 0: Black
    {255, 220, 0},    // 1: Yellow
    {227, 27, 35},    // 2: Red / Magenta
    {0, 128, 255},    // 3: Blue / Cyan
    {255, 128, 0},    // 4: Orange (Yellow + Red)
    {0, 176, 80},     // 5: Green (Yellow + Blue)
    {112, 48, 160},   // 6: Purple (Red + Blue)
    {0, 0, 0}         // 7: Default Black
};

// Minimal 5x7 dot-matrix font for ASCII 32..126
#include "font5x7.h"

typedef enum {
    IW_STATE_TEXT,
    IW_STATE_ESC,
    IW_STATE_LINE_PITCH_1,
    IW_STATE_LINE_PITCH_2,
    IW_STATE_COLOR,
    IW_STATE_GRAPHICS_LEN,
    IW_STATE_GRAPHICS_DATA
} IWState;

static struct {
    IWState state;
    char g_cmd;        // 'G' (72 DPI) or 'S' (144 DPI) or 'g'
    int g_cols_expected;
    int g_cols_read;
    char g_len_str[5];
    int g_len_idx;
    int pitch_val;
    int dpi_mode;
} iw;

void parser_imagewriter_init(JobState *job) {
    (void)job;
    iw.state = IW_STATE_TEXT;
    iw.g_cols_expected = 0;
    iw.g_cols_read = 0;
    iw.g_len_idx = 0;
    iw.dpi_mode = 72;
    job->canvas.line_spacing = (job->canvas.dpi * 24) / 144; // 24/144" = 1/6"
    job->canvas.cur_color = IW_COLORS[0];
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
    c->head_x += 6 * scale; // 5 dots + 1 dot space
}

void parser_imagewriter_byte(JobState *job, uint8_t byte) {
    Canvas *c = &job->canvas;
    int margin_left = (int)(0.5f * c->dpi);
    int margin_bottom = c->height - (int)(0.5f * c->dpi);

    switch (iw.state) {
        case IW_STATE_TEXT:
            if (byte == 0x1B) { // ESC
                iw.state = IW_STATE_ESC;
            } else if (byte == 0x0D) { // CR
                c->head_x = margin_left;
            } else if (byte == 0x0A) { // LF
                c->head_y += c->line_spacing;
                if (c->head_y >= margin_bottom) {
                    job_commit_page(job);
                }
            } else if (byte == 0x0C) { // Form Feed
                job_commit_page(job);
            } else if (byte >= 32 && byte <= 126) {
                draw_char(c, (char)byte);
            }
            break;

        case IW_STATE_ESC:
            if (byte == 'T') {
                iw.state = IW_STATE_LINE_PITCH_1;
            } else if (byte == 'K') {
                iw.state = IW_STATE_COLOR;
            } else if (byte == 'G' || byte == 'S' || byte == 'g') {
                iw.g_cmd = (char)byte;
                iw.g_len_idx = 0;
                iw.state = IW_STATE_GRAPHICS_LEN;
            } else if (byte == 'n') {
                iw.dpi_mode = 72;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'N') {
                iw.dpi_mode = 80;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'E') {
                iw.dpi_mode = 96;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'e') {
                iw.dpi_mode = 107;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'q') {
                iw.dpi_mode = 120;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'Q') {
                iw.dpi_mode = 136;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'p') {
                iw.dpi_mode = 144;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'P') {
                iw.dpi_mode = 160;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'A') { // 1/6"
                c->line_spacing = c->dpi / 6;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'B') { // 1/8"
                c->line_spacing = c->dpi / 8;
                iw.state = IW_STATE_TEXT;
            } else if (byte == 'c' || byte == '?') { // Reset
                c->line_spacing = c->dpi / 6;
                c->cur_color = IW_COLORS[0];
                iw.dpi_mode = 72;
                iw.state = IW_STATE_TEXT;
            } else {
                // Other escape sequence without parameters ('>', '<', '!', '"', 'X', 'Y', etc.)
                iw.state = IW_STATE_TEXT;
            }
            break;

        case IW_STATE_LINE_PITCH_1:
            if (isdigit(byte)) {
                iw.pitch_val = (byte - '0') * 10;
                iw.state = IW_STATE_LINE_PITCH_2;
            } else {
                iw.state = IW_STATE_TEXT;
            }
            break;

        case IW_STATE_LINE_PITCH_2:
            if (isdigit(byte)) {
                iw.pitch_val += (byte - '0');
                // Pitch is in 1/144" units
                c->line_spacing = (job->canvas.dpi * iw.pitch_val) / 144;
            }
            iw.state = IW_STATE_TEXT;
            break;

        case IW_STATE_COLOR: {
            int col = (byte >= '0' && byte <= '6') ? (byte - '0') : 0;
            c->cur_color = IW_COLORS[col];
            iw.state = IW_STATE_TEXT;
            break;
        }

        case IW_STATE_GRAPHICS_LEN: {
            char ch = (char)byte;
            if (ch == ' ') ch = '0';
            int target_len = (iw.g_cmd == 'g') ? 3 : 4;
            if (isdigit((unsigned char)ch)) {
                iw.g_len_str[iw.g_len_idx++] = ch;
                if (iw.g_len_idx == target_len) {
                    iw.g_len_str[target_len] = '\0';
                    iw.g_cols_expected = atoi(iw.g_len_str);
                    iw.g_cols_read = 0;
                    if (iw.g_cols_expected > 0) {
                        iw.state = IW_STATE_GRAPHICS_DATA;
                    } else {
                        iw.state = IW_STATE_TEXT;
                    }
                }
            } else {
                iw.state = IW_STATE_TEXT;
            }
            break;
        }

        case IW_STATE_GRAPHICS_DATA: {
            int scale_x = (iw.g_cmd == 'S' || iw.dpi_mode >= 144) ? (c->dpi / 144) : (c->dpi / 72);
            int v_scale = c->dpi / 72; // 72 DPI vertical pin spacing = 2 px at 144 DPI
            if (scale_x < 1) scale_x = 1;
            if (v_scale < 1) v_scale = 1;

            // Bit 0 is top dot (Pin 1), Bit 7 is bottom dot (Pin 8)
            for (int pin = 0; pin < 8; pin++) {
                if (byte & (1 << pin)) {
                    int py = c->head_y + (pin * v_scale);
                    for (int dx = 0; dx < scale_x; dx++) {
                        for (int dy = 0; dy < v_scale; dy++) {
                            canvas_plot_dot(c, c->head_x + dx, py + dy, c->cur_color);
                        }
                    }
                }
            }
            c->head_x += scale_x;
            iw.g_cols_read++;

            if (iw.g_cols_read >= iw.g_cols_expected) {
                iw.state = IW_STATE_TEXT;
            }
            break;
        }
    }
}

