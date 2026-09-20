#include "printer.h"
#include "pdfgen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

bool job_start(JobState *job, const PrinterConfig *cfg) {
    if (job->job_active) return true;

    // Ensure output directory exists
    struct stat st;
    if (stat(cfg->output_dir, &st) == -1) {
        mkdir(cfg->output_dir, 0755);
    }

    // Format timestamp filename: Print_YYYY-MM-DD_HH-MM-SS.pdf (with deduplication)
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char base_name[128];
    strftime(base_name, sizeof(base_name), "Print_%Y-%m-%d_%H-%M-%S", t);
    snprintf(job->current_job_path, sizeof(job->current_job_path), "%s/%s.pdf", cfg->output_dir, base_name);

    int dup_idx = 1;
    while (stat(job->current_job_path, &st) == 0) {
        snprintf(job->current_job_path, sizeof(job->current_job_path), "%s/%s_%d.pdf", cfg->output_dir, base_name, dup_idx++);
    }

    struct pdf_info info = {
        .creator = "MiSTer FPGA Retro Printer Emulation",
        .producer = "mister_printerd",
        .title = "Retro Printout",
        .author = "MiSTer FPGA",
        .subject = "Emulated Dot Matrix / Serial Output",
        .date = ""
    };
    strftime(info.date, sizeof(info.date), "%Y-%m-%d %H:%M:%S", t);

    float page_w = (cfg->paper_size == PAPER_A4) ? PDF_A4_WIDTH : PDF_LETTER_WIDTH;
    float page_h = (cfg->paper_size == PAPER_A4) ? PDF_A4_HEIGHT : PDF_LETTER_HEIGHT;

    job->pdf_doc = (void *)pdf_create(page_w, page_h, &info);
    if (!job->pdf_doc) {
        fprintf(stderr, "Error: Failed to create PDF document.\n");
        return false;
    }

    job->page_count = 0;
    job->job_active = true;
    job->last_data_time = (long)now;
    canvas_clear(&job->canvas);

    if (cfg->verbose) {
        printf("[printerd] Started new print job: %s\n", job->current_job_path);
    }
    return true;
}

bool job_commit_page(JobState *job) {
    if (!job->job_active || !job->canvas.dirty) return true;

    struct pdf_doc *pdf = (struct pdf_doc *)job->pdf_doc;
    struct pdf_object *page = pdf_append_page(pdf);

    float page_w = (job->paper_size == PAPER_A4) ? PDF_A4_WIDTH : PDF_LETTER_WIDTH;
    float page_h = (job->paper_size == PAPER_A4) ? PDF_A4_HEIGHT : PDF_LETTER_HEIGHT;

    // Embed current raster canvas at native point size
    pdf_add_rgb24(pdf, page, 0, 0, page_w, page_h,
                  job->canvas.buffer, job->canvas.width, job->canvas.height);

    job->page_count++;
    printf("[printerd] Committed Page %d (%dx%d px @ %d DPI)\n",
           job->page_count, job->canvas.width, job->canvas.height, job->canvas.dpi);

    canvas_clear(&job->canvas);
    return true;
}

bool job_finalize(JobState *job, const PrinterConfig *cfg) {
    if (!job->job_active) return true;

    // Commit any lingering graphics on the current page
    if (job->canvas.dirty) {
        job_commit_page(job);
    }

    if (job->page_count == 0) {
        // If no pages were printed (e.g. spurious stray characters), cancel job
        if (cfg->verbose) {
            printf("[printerd] Job was empty, discarding.\n");
        }
        pdf_destroy((struct pdf_doc *)job->pdf_doc);
        job->pdf_doc = NULL;
        job->job_active = false;
        return true;
    }

    struct pdf_doc *pdf = (struct pdf_doc *)job->pdf_doc;
    int ret = pdf_save(pdf, job->current_job_path);
    if (ret < 0) {
        fprintf(stderr, "[printerd] Error saving PDF '%s': %s\n",
                job->current_job_path, pdf_get_err(pdf, NULL));
    } else {
        printf("[printerd] Successfully saved PDF: %s (%d page%s)\n",
               job->current_job_path, job->page_count, job->page_count == 1 ? "" : "s");
    }

    pdf_destroy(pdf);
    job->pdf_doc = NULL;
    job->job_active = false;
    job->page_count = 0;
    return (ret >= 0);
}
