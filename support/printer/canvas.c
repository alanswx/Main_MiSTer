#include "printer.h"
#include <stdlib.h>
#include <string.h>

void canvas_init(Canvas *c, int dpi, PaperSize paper) {
    c->dpi = dpi;
    if (paper == PAPER_A4) {
        c->width = (int)(8.267f * dpi);   // 210 mm
        c->height = (int)(11.692f * dpi); // 297 mm
    } else {
        c->width = (int)(8.5f * dpi);     // Letter width
        c->height = (int)(11.0f * dpi);   // Letter height
    }

    size_t sz = (size_t)c->width * c->height * 3;
    c->buffer = (uint8_t *)malloc(sz);
    if (!c->buffer) {
        fprintf(stderr, "Fatal: Unable to allocate canvas memory (%zu bytes)\n", sz);
        exit(1);
    }

    c->line_spacing = dpi / 6; // default 1/6"
    c->cur_color = (RGBColor){0, 0, 0}; // Black
    c->tps_first_line = true;
    canvas_clear(c);
}

void canvas_clear(Canvas *c) {
    size_t sz = (size_t)c->width * c->height * 3;
    memset(c->buffer, 0xFF, sz); // Pure white background
    c->head_x = (int)(0.5f * c->dpi); // 0.5" left margin
    c->head_y = (int)(0.5f * c->dpi); // 0.5" top margin
    c->dirty = false;
}

void canvas_free(Canvas *c) {
    if (c->buffer) {
        free(c->buffer);
        c->buffer = NULL;
    }
}

void canvas_plot_dot(Canvas *c, int x, int y, RGBColor col) {
    if (x < 0 || x >= c->width || y < 0 || y >= c->height) return;
    int idx = (y * c->width + x) * 3;
    
    // Subtractive / CMYK optical ink blending on white paper
    c->buffer[idx + 0] = (uint8_t)((c->buffer[idx + 0] * col.r) / 255);
    c->buffer[idx + 1] = (uint8_t)((c->buffer[idx + 1] * col.g) / 255);
    c->buffer[idx + 2] = (uint8_t)((c->buffer[idx + 2] * col.b) / 255);
    c->dirty = true;
}

void canvas_plot_rect(Canvas *c, int x, int y, int w, int h, RGBColor col) {
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            canvas_plot_dot(c, x + dx, y + dy, col);
        }
    }
}
