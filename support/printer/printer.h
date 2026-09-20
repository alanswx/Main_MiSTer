#ifndef PRINTER_H
#define PRINTER_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

#define DEFAULT_DPI 144
#define DEFAULT_TIMEOUT_SEC 4
#define DEFAULT_BAUD 9600
#define DEFAULT_DEVICE "/dev/ttyS1"
#define DEFAULT_OUTPUT_DIR "/media/fat/printers"

typedef enum {
    MODEL_IMAGEWRITER,
    MODEL_EPSON,
    MODEL_EPSON_TPS,
    MODEL_ADAM,
    MODEL_MPS803
} PrinterModel;

typedef enum {
    PAPER_LETTER,
    PAPER_A4
} PaperSize;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} RGBColor;

// Canvas representation (raster buffer for 1 page)
typedef struct {
    int width;           // pixels
    int height;          // pixels
    int dpi;             // dots per inch (e.g. 144)
    uint8_t *buffer;     // RGB24 [height * width * 3]
    int head_x;          // head x position in pixels
    int head_y;          // head y position in pixels
    int line_spacing;    // in canvas pixels
    RGBColor cur_color;
    bool dirty;
    bool tps_first_line; // used by epsonTPS to absorb initial blank CR/LF
} Canvas;

// Active Print Job State
typedef struct {
    PrinterModel model;
    PaperSize paper_size;
    int dpi;
    Canvas canvas;
    void *pdf_doc;       // opaque struct pdf_doc*
    int page_count;
    char current_job_path[512];
    bool job_active;
    long last_data_time; // timestamp of last received byte (seconds)
} JobState;

typedef struct {
    char device[256];
    int baud;
    PrinterModel model;
    char output_dir[512];
    int timeout_sec;
    PaperSize paper_size;
    int dpi;
    bool save_png;
    bool verbose;
    bool daemon_mode;
} PrinterConfig;

// Function declarations
// Canvas & Color
void canvas_init(Canvas *c, int dpi, PaperSize paper);
void canvas_clear(Canvas *c);
void canvas_free(Canvas *c);
void canvas_plot_dot(Canvas *c, int x, int y, RGBColor col);
void canvas_plot_rect(Canvas *c, int x, int y, int w, int h, RGBColor col);

// PDF Output
bool job_start(JobState *job, const PrinterConfig *cfg);
bool job_commit_page(JobState *job);
bool job_finalize(JobState *job, const PrinterConfig *cfg);

// Protocol Parsers
void parser_imagewriter_init(JobState *job);
void parser_imagewriter_byte(JobState *job, uint8_t byte);

void parser_escp_init(JobState *job, bool tps_mode);
void parser_escp_byte(JobState *job, uint8_t byte);

void parser_adam_init(JobState *job);
void parser_adam_byte(JobState *job, uint8_t byte);

void parser_mps803_init(JobState *job);
void parser_mps803_byte(JobState *job, uint8_t byte);

#endif // PRINTER_H
