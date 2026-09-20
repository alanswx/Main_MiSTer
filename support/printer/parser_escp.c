#include "printer.h"
#include <ctype.h>
#include <string.h>

#include "font5x7.h"

typedef enum {
    ESCP_STATE_TEXT,
    ESCP_STATE_ESC,
    ESCP_STATE_LINE_3,
    ESCP_STATE_LINE_A,
    ESCP_STATE_FEED_J,
    ESCP_STATE_FEED_j,
    ESCP_STATE_GFX_NL,
    ESCP_STATE_GFX_NH,
    ESCP_STATE_GFX_DATA,
    ESCP_STATE_STAR_M,
    ESCP_STATE_STAR_NL,
    ESCP_STATE_STAR_NH
} ESCPState;

static struct {
    ESCPState state;
    bool tps_mode;
    char gfx_cmd;
    uint8_t star_m;
    int gfx_cols_expected;
    int gfx_cols_read;
    uint8_t nL;
    uint8_t nH;
    float step_x;
    int pin_count; // 8 or 24
} escp;

void parser_escp_init(JobState *job, bool tps_mode) {
    (void)job;
    escp.state = ESCP_STATE_TEXT;
    escp.tps_mode = tps_mode;
    escp.gfx_cols_expected = 0;
    escp.gfx_cols_read = 0;
    escp.pin_count = 8;
    job->canvas.line_spacing = job->canvas.dpi / 6; // default 1/6"
    job->canvas.cur_color = (RGBColor){0, 0, 0};
    job->canvas.tps_first_line = true;
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

void parser_escp_byte(JobState *job, uint8_t byte) {
    Canvas *c = &job->canvas;
    int margin_left = (int)(0.5f * c->dpi);
    int margin_bottom = c->height - (int)(0.5f * c->dpi);

    switch (escp.state) {
        case ESCP_STATE_TEXT:
            if (byte == 0x1B) { // ESC
                escp.state = ESCP_STATE_ESC;
            } else if (byte == 0x0D) { // CR
                c->head_x = margin_left;
            } else if (byte == 0x0A) { // LF
                if (escp.tps_mode && c->tps_first_line) {
                    // Absorb Print Shop's initial empty line feed
                    c->tps_first_line = false;
                } else {
                    c->head_y += c->line_spacing;
                    if (c->head_y >= margin_bottom) {
                        job_commit_page(job);
                    }
                }
            } else if (byte == 0x0C) { // FF
                job_commit_page(job);
                c->tps_first_line = true;
            } else if (byte >= 32 && byte <= 126) {
                c->tps_first_line = false;
                draw_char(c, (char)byte);
            }
            break;

        case ESCP_STATE_ESC:
            if (byte == '@') { // Initialize
                c->line_spacing = c->dpi / 6;
                c->cur_color = (RGBColor){0, 0, 0};
                c->head_x = margin_left;
                escp.state = ESCP_STATE_TEXT;
            } else if (byte == '3') { // ESC 3 n: n/216"
                escp.state = ESCP_STATE_LINE_3;
            } else if (byte == 'A') { // ESC A n: n/72"
                escp.state = ESCP_STATE_LINE_A;
            } else if (byte == '2') { // 1/6"
                c->line_spacing = c->dpi / 6;
                escp.state = ESCP_STATE_TEXT;
            } else if (byte == '0') { // 1/8"
                c->line_spacing = c->dpi / 8;
                escp.state = ESCP_STATE_TEXT;
            } else if (byte == '1') { // 7/72"
                c->line_spacing = (c->dpi * 7) / 72;
                escp.state = ESCP_STATE_TEXT;
            } else if (byte == 'J') { // ESC J n: Immediate LF
                escp.state = ESCP_STATE_FEED_J;
            } else if (byte == 'j') { // ESC j n: Immediate reverse LF
                escp.state = ESCP_STATE_FEED_j;
            } else if (byte == 'K' || byte == 'L' || byte == 'Y' || byte == 'Z') {
                escp.gfx_cmd = byte;
                escp.pin_count = 8;
                escp.state = ESCP_STATE_GFX_NL;
            } else if (byte == '*') {
                escp.state = ESCP_STATE_STAR_M;
            } else {
                // Ignore other escapes
                escp.state = ESCP_STATE_TEXT;
            }
            break;

        case ESCP_STATE_LINE_3:
            // n/216 inch
            c->line_spacing = (int)(((float)byte * c->dpi) / 216.0f + 0.5f);
            escp.state = ESCP_STATE_TEXT;
            break;

        case ESCP_STATE_LINE_A:
            // n/72 inch
            c->line_spacing = (int)(((float)byte * c->dpi) / 72.0f + 0.5f);
            escp.state = ESCP_STATE_TEXT;
            break;

        case ESCP_STATE_FEED_J:
            c->head_y += (int)(((float)byte * c->dpi) / 216.0f + 0.5f);
            escp.state = ESCP_STATE_TEXT;
            break;

        case ESCP_STATE_FEED_j:
            c->head_y -= (int)(((float)byte * c->dpi) / 216.0f + 0.5f);
            if (c->head_y < 0) c->head_y = 0;
            escp.state = ESCP_STATE_TEXT;
            break;

        case ESCP_STATE_GFX_NL:
            escp.nL = byte;
            escp.state = ESCP_STATE_GFX_NH;
            break;

        case ESCP_STATE_GFX_NH:
            escp.nH = byte;
            escp.gfx_cols_expected = escp.nL | ((int)(escp.nH & 0x07) << 8);
            escp.gfx_cols_read = 0;
            // Density steps
            if (escp.gfx_cmd == 'K') {
                escp.step_x = (float)c->dpi / 60.0f;  // 60 DPI
            } else if (escp.gfx_cmd == 'Z') {
                escp.step_x = (float)c->dpi / 240.0f; // 240 DPI
            } else {
                escp.step_x = (float)c->dpi / 120.0f; // 120 DPI (L, Y)
            }
            if (escp.step_x < 1.0f) escp.step_x = 1.0f;

            if (escp.gfx_cols_expected > 0) {
                escp.state = ESCP_STATE_GFX_DATA;
            } else {
                escp.state = ESCP_STATE_TEXT;
            }
            break;

        case ESCP_STATE_STAR_M:
            escp.star_m = byte;
            escp.pin_count = (byte >= 32) ? 24 : 8;
            escp.state = ESCP_STATE_STAR_NL;
            break;

        case ESCP_STATE_STAR_NL:
            escp.nL = byte;
            escp.state = ESCP_STATE_STAR_NH;
            break;

        case ESCP_STATE_STAR_NH:
            escp.nH = byte;
            escp.gfx_cols_expected = escp.nL | ((int)escp.nH << 8);
            escp.gfx_cols_read = 0;
            if (escp.star_m == 0 || escp.star_m == 32) {
                escp.step_x = (float)c->dpi / 60.0f;
            } else if (escp.star_m == 1 || escp.star_m == 33) {
                escp.step_x = (float)c->dpi / 120.0f;
            } else if (escp.star_m == 3 || escp.star_m == 39) {
                escp.step_x = (float)c->dpi / 180.0f;
            } else {
                escp.step_x = (float)c->dpi / 120.0f;
            }
            if (escp.step_x < 1.0f) escp.step_x = 1.0f;

            if (escp.gfx_cols_expected > 0) {
                escp.state = ESCP_STATE_GFX_DATA;
            } else {
                escp.state = ESCP_STATE_TEXT;
            }
            break;

        case ESCP_STATE_GFX_DATA: {
            c->tps_first_line = false;
            int v_scale = c->dpi / 72; // Standard 72 DPI vertical pin spacing = 2 px at 144 DPI
            if (v_scale < 1) v_scale = 1;
            int w = (int)escp.step_x;
            if (w < 1) w = 1;

            int px = (int)(c->head_x + escp.gfx_cols_read * escp.step_x);

            // Bit 7 is top dot (Pin 1), Bit 0 is bottom dot (Pin 8)
            for (int pin = 0; pin < 8; pin++) {
                if (byte & (1 << (7 - pin))) {
                    int py = c->head_y + (pin * v_scale);
                    for (int dx = 0; dx < w; dx++) {
                        for (int dy = 0; dy < v_scale; dy++) {
                            canvas_plot_dot(c, px + dx, py + dy, c->cur_color);
                        }
                    }
                }
            }
            escp.gfx_cols_read++;

            if (escp.gfx_cols_read >= escp.gfx_cols_expected) {
                c->head_x += (int)(escp.gfx_cols_expected * escp.step_x);
                escp.state = ESCP_STATE_TEXT;
            }
            break;
        }
    }
}

