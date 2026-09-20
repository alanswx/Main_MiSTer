/**
 * mister_printerd - MiSTer FPGA Retro Printer Emulation Daemon
 * Translates serial / parallel retro printer streams into PDF documents.
 */

#include "printer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <time.h>
#include <sys/poll.h>
#include <sys/stat.h>

static volatile bool running = true;

static void sig_handler(int sig) {
    (void)sig;
    running = false;
}

static speed_t baud_to_speed(int baud) {
    switch (baud) {
        case 1200:  return B1200;
        case 2400:  return B2400;
        case 4800:  return B4800;
        case 9600:  return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
        default:    return B9600;
    }
}

static int open_serial_port(const char *path, int baud) {
    if (strcmp(path, "-") == 0 || strcmp(path, "/dev/stdin") == 0) {
        return STDIN_FILENO;
    }

    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            perror("[printerd] Error opening device/file");
            return -1;
        }
    }

    if (!isatty(fd)) {
        // Regular file or pipe, skip termios configuration
        return fd;
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    if (tcgetattr(fd, &tio) != 0) {
        perror("[printerd] tcgetattr failed");
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);
    speed_t spd = baud_to_speed(baud);
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    // 8N1 + hardware flow control
    tio.c_cflag |= (CS8 | CLOCAL | CREAD);
    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
#ifdef CRTSCTS
    tio.c_cflag |= CRTSCTS; // RTS/CTS hardware handshake
#endif

    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 0;

    tcflush(fd, TCIFLUSH);
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        perror("[printerd] tcsetattr failed");
        close(fd);
        return -1;
    }

    return fd;
}

static void print_usage(const char *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -d <dev>       Serial device path (default: %s or '-' for stdin)\n", DEFAULT_DEVICE);
    printf("  -b <baud>      Baud rate (default: %d)\n", DEFAULT_BAUD);
    printf("  -m <model>     Printer model: imagewriter | epson | epson-tps | adam | mps803 (default: imagewriter)\n");
    printf("  -o <dir>       Output directory for PDFs (default: %s)\n", DEFAULT_OUTPUT_DIR);
    printf("  -t <sec>       Inactivity timeout in seconds to commit job (default: %d)\n", DEFAULT_TIMEOUT_SEC);
    printf("  -s <size>      Paper size: letter | a4 (default: letter)\n");
    printf("  -r <dpi>       Canvas DPI: 144 | 288 (default: %d)\n", DEFAULT_DPI);
    printf("  -B             Run as background daemon\n");
    printf("  -v             Verbose output\n");
    printf("  -h             Show this help message\n");
}

static void reset_model_parser(JobState *job) {
    switch (job->model) {
        case MODEL_IMAGEWRITER:
            parser_imagewriter_init(job);
            break;
        case MODEL_EPSON:
            parser_escp_init(job, false);
            break;
        case MODEL_EPSON_TPS:
            parser_escp_init(job, true);
            break;
        case MODEL_ADAM:
            parser_adam_init(job);
            break;
        case MODEL_MPS803:
            parser_mps803_init(job);
            break;
    }
}

static void dispatch_byte(JobState *job, uint8_t byte) {
    switch (job->model) {
        case MODEL_IMAGEWRITER:
            parser_imagewriter_byte(job, byte);
            break;
        case MODEL_EPSON:
        case MODEL_EPSON_TPS:
            parser_escp_byte(job, byte);
            break;
        case MODEL_ADAM:
            parser_adam_byte(job, byte);
            break;
        case MODEL_MPS803:
            parser_mps803_byte(job, byte);
            break;
    }
}

int main(int argc, char **argv) {
    PrinterConfig cfg = {
        .device = DEFAULT_DEVICE,
        .baud = DEFAULT_BAUD,
        .model = MODEL_IMAGEWRITER,
        .output_dir = DEFAULT_OUTPUT_DIR,
        .timeout_sec = DEFAULT_TIMEOUT_SEC,
        .paper_size = PAPER_LETTER,
        .dpi = DEFAULT_DPI,
        .save_png = false,
        .verbose = false,
        .daemon_mode = false
    };

    // If /media/fat doesn't exist (e.g. testing on host PC/Mac), use ./printers
    struct stat st;
    if (stat("/media/fat", &st) == -1) {
        strcpy(cfg.output_dir, "./printers");
    }

    int opt;
    while ((opt = getopt(argc, argv, "d:b:m:o:t:s:r:Bvh")) != -1) {
        switch (opt) {
            case 'd': strncpy(cfg.device, optarg, sizeof(cfg.device) - 1); break;
            case 'b': cfg.baud = atoi(optarg); break;
            case 'm':
                if (strcasecmp(optarg, "epson") == 0) cfg.model = MODEL_EPSON;
                else if (strcasecmp(optarg, "epson-tps") == 0) cfg.model = MODEL_EPSON_TPS;
                else if (strcasecmp(optarg, "adam") == 0) cfg.model = MODEL_ADAM;
                else if (strcasecmp(optarg, "mps803") == 0) cfg.model = MODEL_MPS803;
                else cfg.model = MODEL_IMAGEWRITER;
                break;
            case 'o': strncpy(cfg.output_dir, optarg, sizeof(cfg.output_dir) - 1); break;
            case 't': cfg.timeout_sec = atoi(optarg); break;
            case 's':
                if (strcasecmp(optarg, "a4") == 0) cfg.paper_size = PAPER_A4;
                else cfg.paper_size = PAPER_LETTER;
                break;
            case 'r': cfg.dpi = atoi(optarg); break;
            case 'B': cfg.daemon_mode = true; break;
            case 'v': cfg.verbose = true; break;
            case 'h': print_usage(argv[0]); return 0;
            default: print_usage(argv[0]); return 1;
        }
    }

    if (cfg.daemon_mode) {
#if defined(__APPLE__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        if (daemon(0, 0) != 0) {
            perror("[printerd] Failed to daemonize");
            return 1;
        }
#pragma clang diagnostic pop
#else
        if (daemon(0, 0) != 0) {
            perror("[printerd] Failed to daemonize");
            return 1;
        }
#endif
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("[printerd] Starting mister_printerd (Model: %d, Baud: %d, Device: %s, Out: %s)\n",
           cfg.model, cfg.baud, cfg.device, cfg.output_dir);

    int fd = open_serial_port(cfg.device, cfg.baud);
    if (fd < 0) {
        return 1;
    }

    JobState job;
    memset(&job, 0, sizeof(job));
    job.model = cfg.model;
    job.paper_size = cfg.paper_size;
    job.dpi = cfg.dpi;
    canvas_init(&job.canvas, cfg.dpi, cfg.paper_size);
    reset_model_parser(&job);

    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLIN;

    uint8_t read_buf[512];

    while (running) {
        int poll_res = poll(&pfd, 1, 500); // 500ms timeout
        time_t now = time(NULL);

        if (poll_res > 0 && (pfd.revents & POLLIN)) {
            ssize_t n = read(fd, read_buf, sizeof(read_buf));
            if (n > 0) {
                if (!job.job_active) {
                    job_start(&job, &cfg);
                    reset_model_parser(&job);
                }
                job.last_data_time = (long)now;

                for (ssize_t i = 0; i < n; i++) {
                    dispatch_byte(&job, read_buf[i]);
                }
            } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                if (cfg.verbose) perror("[printerd] read error");
                break;
            } else if (n == 0) {
                if (!isatty(fd) || strcmp(cfg.device, "-") == 0 || strcmp(cfg.device, "/dev/stdin") == 0) {
                    if (cfg.verbose) printf("[printerd] End of input file/pipe reached.\n");
                    break;
                }
            }
        }

        // Inactivity timeout check: flush active print job to PDF
        if (job.job_active) {
            if ((long)now - job.last_data_time >= cfg.timeout_sec) {
                if (cfg.verbose) printf("[printerd] Inactivity timeout reached (%ds), finalizing job...\n", cfg.timeout_sec);
                job_finalize(&job, &cfg);
                reset_model_parser(&job);
            }
        }
    }

    // Flush any pending job on termination
    if (job.job_active) {
        job_finalize(&job, &cfg);
    }

    canvas_free(&job.canvas);
    if (fd != STDIN_FILENO) {
        close(fd);
    }

    printf("[printerd] Shutdown complete.\n");
    return 0;
}
