/*
 * inmpTest - userspace client that records raw 16-bit mono PCM from an
 * ESP32 + INMP441 microphone over a USB-serial port and saves it as WAV.
 *
 * Default mode: the device is read directly from the stock USB-serial
 * driver (e.g. /dev/ttyUSB0), configured here, and framed packets are
 * parsed in userspace.
 *
 * Line-discipline mode (-l): inmp-module is attached with
 *   ldattach -s 921600 29 /dev/ttyUSB0
 * and read() already returns clean PCM (parsed, CRC-checked, zero-filled),
 * so termios setup and packet parsing are skipped.
 *
 * Usage: inmpTest [-l] [device]
 *
 * Commands (type the letter, then Enter):
 *   idle:       s = start recording, q = quit
 *   recording:  t = stop and save,   c = cancel and discard
 * Ctrl+C / SIGTERM / SIGHUP cancel any active recording and exit.
 *
 * Build: gcc -std=gnu17 -Wall -Wextra -O2 -o inmpTest inmpTest.c
 */

#define _DEFAULT_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "../../inmp-module/inmp_ioctl.h"

/* ---- Configuration: must match what the ESP32 firmware sends ---- */
#define SERIAL_DEVICE    "/dev/ttyUSB0"
#define SERIAL_BAUD      B921600
#define SAMPLE_RATE      16000u
#define NUM_CHANNELS     1u
#define BITS_PER_SAMPLE  16u

#define OUTPUT_WAV       "/tmp/inmpTest.wav"
#define TEMP_TEMPLATE    "/tmp/inmpTest-XXXXXX"

#define FRAME_BYTES      (NUM_CHANNELS * BITS_PER_SAMPLE / 8u)
#define WAV_HEADER_SIZE  44u
#define MAX_DATA_BYTES   (UINT32_MAX - 36u)

#define CMD_EOF  (-1)
#define CMD_NONE (0)

#define LDISC_INMP       29   /* N_DEVELOPMENT, used by inmp-module */
#define NO_DATA_WARN_SEC 3

static const char *g_device = SERIAL_DEVICE;
static bool g_ldisc = false; /* -l: kernel line discipline does the parsing */

/* Only ever touched by the signal handler (write) and normal code (read). */
static volatile sig_atomic_t g_abort = 0;

static void onSignal(int sig)
{
    (void)sig;
    g_abort = 1;
}

static bool installSignalHandlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onSignal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* deliberately no SA_RESTART: blocking calls return EINTR */

    const int sigs[] = { SIGINT, SIGTERM, SIGHUP };
    for (size_t i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        if (sigaction(sigs[i], &sa, NULL) != 0) {
            perror("sigaction");
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Serial port                                                         */
/* ------------------------------------------------------------------ */

static int configureSerialPort(int fd, speed_t baud)
{
    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        perror("tcgetattr");
        return -1;
    }

    cfmakeraw(&tty); /* clears ICRNL/INLCR/IGNCR/ISTRIP/ */
    if (cfsetspeed(&tty, baud) != 0) {
        perror("cfsetspeed");
        return -1;
    }

    tty.c_cflag &= ~(CSTOPB | CRTSCTS); /* 1 stop bit, no hardware flow control */
    tty.c_cflag |= CREAD | CLOCAL;
    tty.c_cc[VMIN]  = 0;
    tty.c_cc[VTIME] = 1; /* 0.1 s read timeout; poll() does the real waiting */

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        perror("tcsetattr");
        return -1;
    }

    tcflush(fd, TCIFLUSH); /* drop stale bytes, e.g. ESP32 boot log */
    return 0;
}

static int openSerial(void)
{
    int fd = open(g_device, O_RDONLY | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Failed to open %s: %s\n", g_device, strerror(errno));
        return -1;
    }
    if (g_ldisc) {
        /* ldattach already set baud/raw mode; just confirm it is attached. */
        int ldisc = -1;
        if (ioctl(fd, TIOCGETD, &ldisc) != 0 || ldisc != LDISC_INMP) {
            fprintf(stderr,
                    "%s is not using the inmp441 line discipline (found %d, need %d).\n"
                    "Run: sudo insmod inmp-module.ko && sudo ldattach -s 921600 %d %s\n",
                    g_device, ldisc, LDISC_INMP, LDISC_INMP, g_device);
            close(fd);
            return -1;
        }
    } else if (configureSerialPort(fd, SERIAL_BAUD) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* ------------------------------------------------------------------ */
/* WAV output (explicit little-endian, independent of host byte order) */
/* ------------------------------------------------------------------ */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void buildWavHeader(uint8_t h[WAV_HEADER_SIZE], uint32_t dataSize)
{
    memcpy(h + 0, "RIFF", 4);
    put32(h + 4, 36u + dataSize);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put32(h + 16, 16);                       /* PCM fmt chunk size */
    put16(h + 20, 1);                        /* PCM */
    put16(h + 22, NUM_CHANNELS);
    put32(h + 24, SAMPLE_RATE);
    put32(h + 28, SAMPLE_RATE * FRAME_BYTES); /* byte rate */
    put16(h + 32, FRAME_BYTES);              /* block align */
    put16(h + 34, BITS_PER_SAMPLE);
    memcpy(h + 36, "data", 4);
    put32(h + 40, dataSize);
}

/* Patch the header, flush to disk and close. Returns true on success. */
static bool finalizeWav(FILE *out, uint32_t dataBytes)
{
    uint8_t header[WAV_HEADER_SIZE];
    buildWavHeader(header, dataBytes);

    bool ok = fflush(out) == 0
           && ftruncate(fileno(out), (off_t)(WAV_HEADER_SIZE + dataBytes)) == 0
           && fseek(out, 0, SEEK_SET) == 0
           && fwrite(header, 1, sizeof header, out) == sizeof header
           && fflush(out) == 0
           && fsync(fileno(out)) == 0;

    if (!ok)
        perror("Failed to finalize WAV file");
    if (fclose(out) != 0) {
        perror("Failed to close WAV file");
        ok = false;
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* Console commands                                                    */
/* ------------------------------------------------------------------ */

/*
 * Reads one line straight from fd 0 (no stdio buffering, so poll() on
 * STDIN_FILENO stays accurate). Returns the first non-space character,
 * CMD_NONE for a blank line or an interrupted read, CMD_EOF on EOF/error.
 */
static int readCommand(void)
{
    char line[64];
    ssize_t n = read(STDIN_FILENO, line, sizeof line);
    if (n == 0)
        return CMD_EOF;
    if (n < 0)
        return errno == EINTR ? CMD_NONE : CMD_EOF;

    for (ssize_t i = 0; i < n; i++) {
        if (!isspace((unsigned char)line[i]))
            return tolower((unsigned char)line[i]);
    }
    return CMD_NONE;
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

/*
 * Packet format sent by the ESP32 (all multi-byte fields little-endian):
 *
 *   [A5 5A C3 3C]  magic (4 bytes)
 *   [seq]          uint16, +1 per packet, wraps at 65536
 *   [len]          uint16, payload bytes (even, 1..MAX_PAYLOAD)
 *   [payload]      len bytes of 16-bit PCM
 *   [crc]          uint16, CRC-16/CCITT-FALSE over seq + len + payload
 *
 * The parser hunts for the magic, validates length and CRC, and only then
 * accepts the payload. A false magic inside audio fails the CRC and is skipped
 * one byte at a time, so the stream always re-aligns by itself.
 */
#define PKT_MAGIC_LEN  4u
#define PKT_HDR_SIZE   8u   /* magic + seq + len */
#define MAX_PAYLOAD    1024u
#define PKT_MAX_SIZE   (PKT_HDR_SIZE + MAX_PAYLOAD + 2u)
#define MAX_FILL_PKTS  64u  /* larger seq jumps are treated as a device reset */
 
static const uint8_t PKT_MAGIC[PKT_MAGIC_LEN] = { 0xA5, 0x5A, 0xC3, 0x3C };
 
static uint16_t crc16Update(uint16_t crc, const uint8_t *d, size_t n)
{
    while (n--) {
        crc ^= (uint16_t)((uint16_t)(*d++) << 8);
        for (int i = 0; i < 8; i++)
            crc = (uint16_t)((crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1));
    }
    return crc;
}
 
static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
 
typedef struct {
    uint8_t  buf[8192];
    size_t   len;
    uint16_t nextSeq;
    bool     haveSeq;
    uint32_t good, badCrc, lostPkts, skippedBytes;
} Parser;
 
typedef enum { PARSE_OK, PARSE_SIZE_LIMIT, PARSE_WRITE_ERROR } ParseResult;
 
/* Consume as many complete packets as p->buf holds; write payloads to out. */
static ParseResult parserProcess(Parser *p, FILE *out, uint32_t *dataBytes)
{
    static const uint8_t zeros[MAX_PAYLOAD] = { 0 };
    size_t pos = 0;
    ParseResult result = PARSE_OK;
 
    for (;;) {
        while (pos + PKT_MAGIC_LEN <= p->len &&
               memcmp(p->buf + pos, PKT_MAGIC, PKT_MAGIC_LEN) != 0) {
            pos++;
            p->skippedBytes++;
        }
        if (pos + PKT_HDR_SIZE > p->len)
            break; /* need more bytes (keeps a partial magic at the tail) */
 
        uint16_t seq  = get16(p->buf + pos + 4);
        uint16_t plen = get16(p->buf + pos + 6);
        if (plen == 0 || plen > MAX_PAYLOAD || plen % FRAME_BYTES != 0) {
            pos++; /* not a real header */
            p->skippedBytes++;
            continue;
        }
 
        size_t total = PKT_HDR_SIZE + plen + 2u;
        if (pos + total > p->len)
            break; /* packet not fully received yet */
 
        uint16_t want = get16(p->buf + pos + PKT_HDR_SIZE + plen);
        if (crc16Update(0xFFFF, p->buf + pos + 4, 4u + plen) != want) {
            p->badCrc++;
            pos++;
            p->skippedBytes++;
            continue;
        }
 
        /* Valid packet. Keep timing intact if whole packets went missing. */
        if (p->haveSeq && seq != p->nextSeq) {
            uint16_t missing = (uint16_t)(seq - p->nextSeq);
            if (missing <= MAX_FILL_PKTS) {
                p->lostPkts += missing;
                for (uint16_t i = 0; i < missing; i++) {
                    if (plen > MAX_DATA_BYTES - *dataBytes) { result = PARSE_SIZE_LIMIT; goto done; }
                    if (fwrite(zeros, 1, plen, out) != plen) { result = PARSE_WRITE_ERROR; goto done; }
                    *dataBytes += plen;
                }
            }
        }
 
        if (plen > MAX_DATA_BYTES - *dataBytes) { result = PARSE_SIZE_LIMIT; goto done; }
        if (fwrite(p->buf + pos + PKT_HDR_SIZE, 1, plen, out) != plen) {
            result = PARSE_WRITE_ERROR;
            goto done;
        }
        *dataBytes += plen;
        p->good++;
        p->nextSeq = (uint16_t)(seq + 1);
        p->haveSeq = true;
        pos += total;
    }
 
done:
    memmove(p->buf, p->buf + pos, p->len - pos);
    p->len -= pos;
    return result;
}


typedef enum { STOP_SAVE, STOP_CANCEL, STOP_ERROR } StopAction;

/* Returns true if a WAV file was saved. */
static bool recordSession(void)
{
    int serialFd = openSerial();
    if (serialFd < 0)
        return false;

    char tmpPath[] = TEMP_TEMPLATE;
    int outFd = mkstemp(tmpPath); /* unpredictable name, mode 0600 */
    if (outFd < 0) {
        perror("mkstemp");
        close(serialFd);
        return false;
    }
    FILE *out = fdopen(outFd, "w+b");
    if (!out) {
        perror("fdopen");
        close(outFd);
        unlink(tmpPath);
        close(serialFd);
        return false;
    }

    ioctl(serialFd, TIOCSETD, &g_ldisc ? LDISC_INMP : N_TTY);
    ioctl(serialFd, INMP_IOC_START);

    /* Placeholder header; real sizes are patched in at the end. */
    uint8_t header[WAV_HEADER_SIZE];
    buildWavHeader(header, 0);
    if (fwrite(header, 1, sizeof header, out) != sizeof header) {
        perror("Failed to write WAV header");
        fclose(out);
        unlink(tmpPath);
        close(serialFd);
        return false;
    }

    puts("Recording... type 't' + Enter to stop and save, 'c' + Enter to cancel.");

    struct pollfd fds[2] = {
        { .fd = STDIN_FILENO, .events = POLLIN },
        { .fd = serialFd,     .events = POLLIN },
    };

    static Parser parser; /* ~8 KB, keep off the stack */
    memset(&parser, 0, sizeof parser);

    uint32_t dataBytes = 0;
    StopAction action = STOP_SAVE;
    time_t startTime = time(NULL);
    bool warned = false;

    for (;;) {
        if (g_abort) {
            action = STOP_CANCEL;
            break;
        }

        if (!warned && dataBytes == 0 &&
            time(NULL) - startTime >= NO_DATA_WARN_SEC) {
            warned = true;
            fprintf(stderr,
                    "No audio after %d s. Check that the ESP32 is running at 921600 baud%s.\n",
                    NO_DATA_WARN_SEC,
                    g_ldisc ? " and that dmesg shows inmp441 frames ok= counts" : "");
        }

        if (poll(fds, 2, 200) < 0) {
            if (errno == EINTR)
                continue; /* loop re-checks g_abort */
            perror("poll");
            action = STOP_ERROR;
            break;
        }

        /* Serial data first, so nothing buffered is lost on stop. */
        if (fds[1].revents & POLLIN) {
            fprintf(stderr, "\rReceived %u bytes of audio... ", dataBytes);
            ssize_t n = read(serialFd, parser.buf + parser.len,
                             sizeof parser.buf - parser.len);
            if (n > 0 && g_ldisc) {
                /* Already clean PCM from the kernel line discipline. */
                if ((size_t)n > MAX_DATA_BYTES - dataBytes) {
                    puts("WAV size limit (4 GiB) reached; stopping.");
                    break;
                }
                if (fwrite(parser.buf, 1, (size_t)n, out) != (size_t)n) {
                    perror("Failed to write audio data");
                    action = STOP_ERROR;
                    break;
                }
                dataBytes += (uint32_t)n;
            } else if (n > 0) {
                parser.len += (size_t)n;
                ParseResult pr = parserProcess(&parser, out, &dataBytes);
                if (pr == PARSE_SIZE_LIMIT) {
                    puts("WAV size limit (4 GiB) reached; stopping.");
                    break;
                }
                if (pr == PARSE_WRITE_ERROR) {
                    perror("Failed to write audio data");
                    action = STOP_ERROR;
                    break;
                }
            } else if (n < 0 && errno != EINTR && errno != EAGAIN) {
                perror("Serial read failed");
                break; /* keep what we have */
            }
        } else if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            puts("Serial device disconnected; saving what was captured.");
            break;
        }

        if (fds[0].revents & (POLLIN | POLLHUP)) {
            int cmd = readCommand();
            if (cmd == 't' || cmd == CMD_EOF) {
                break;
            } else if (cmd == 'c') {
                action = STOP_CANCEL;
                break;
            }
        }
    }

    if (g_ldisc) {
        ioctl(serialFd, INMP_IOC_STOP);
    }
    close(serialFd);

    if (action != STOP_SAVE) {
        puts(action == STOP_CANCEL ? "Recording canceled; nothing saved."
                                   : "Recording failed; nothing saved.");
        fclose(out);
        unlink(tmpPath);
        return false;
    }

    dataBytes -= dataBytes % FRAME_BYTES; /* never end on a half sample */
    if (dataBytes == 0) {
        puts("No audio received; nothing saved.");
        fclose(out);
        unlink(tmpPath);
        return false;
    }

    if (!finalizeWav(out, dataBytes)) {
        unlink(tmpPath);
        return false;
    }
    if (rename(tmpPath, OUTPUT_WAV) != 0) { /* atomic replace of any old file */
        perror("Failed to move WAV into place");
        unlink(tmpPath);
        return false;
    }

    printf("Saved %u bytes (%.1f s) to %s\n", dataBytes,
           (double)dataBytes / (SAMPLE_RATE * FRAME_BYTES), OUTPUT_WAV);
    return true;
}

/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            g_ldisc = true;
        } else if (argv[i][0] != '-') {
            g_device = argv[i];
        } else {
            fprintf(stderr, "Usage: %s [-l] [device]\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (!installSignalHandlers())
        return EXIT_FAILURE;

    while (!g_abort) {
        printf("Enter 's' to start recording or 'q' to quit: ");
        fflush(stdout);

        int cmd = readCommand();
        if (cmd == CMD_EOF || cmd == 'q')
            break;
        if (cmd == 's')
            recordSession();
        else if (cmd != CMD_NONE)
            puts("Invalid input. Please try again.");
    }

    puts("\nQuitting...");
    return EXIT_SUCCESS;
}
