/*
 * capt-lbp - CUPS backend for Canon CAPT printers (LBP2900 and relatives)
 *
 * Copyright (C) 2026
 * Licensed under the GNU General Public License, version 3 or later.
 *
 * Protocol knowledge (CAPT command set, status layout, Hi-SCoA band
 * compression) comes from the reverse-engineering work of the captdriver
 * project (Alexey Galakhov, Alexei Gordeev, Moses Chong et al.).
 */
#pragma once

#define _GNU_SOURCE
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

static inline uint8_t LO(uint16_t x) { return x & 0xFF; }
static inline uint8_t HI(uint16_t x) { return (x >> 8) & 0xFF; }
static inline uint16_t WORD(const uint8_t *p) { return (uint16_t) (p[0] | (p[1] << 8)); }

/* ---------- logging (log.c) ---------- */
void log_open_file(const char *path);
void log_msg(const char *level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void log_hex(const char *prefix, const uint8_t *buf, size_t len, size_t max);
void cups_state(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define DBG(...)  log_msg("DEBUG", __VA_ARGS__)
#define INFO(...) log_msg("INFO", __VA_ARGS__)
#define WARN(...) log_msg("WARNING", __VA_ARGS__)
#define ERR(...)  log_msg("ERROR", __VA_ARGS__)

extern volatile int g_cancelled;

/* ---------- transport (transport.c) ---------- */
struct capt_device {
	char sysname[64];     /* e.g. "1-1" */
	char model[64];       /* from IEEE 1284 MDL or product string */
	char serial[64];
	char ieee1284[512];
	char lp_node[64];     /* /dev/usb/lpN if usblp is bound */
	int  busnum, devnum;
	int  ifnum;
	int  ep_in, ep_out;
};

int  dev_enumerate(struct capt_device *out, int max);
int  dev_find(const char *uri, struct capt_device *dev);
int  tr_open(const struct capt_device *dev);
int  tr_open_uri_special(const char *uri);  /* capt-sim:/socket for testing */
int  tr_write(const void *buf, size_t len);
ssize_t tr_read(void *buf, size_t len, int timeout_ms);
void tr_drain_input(void);
extern int tr_write_timeout_ms;
void tr_close(void);

/* ---------- CUPS raster reader (raster.c) ---------- */
struct raster_page {
	unsigned hwres[2];
	unsigned margins[2];      /* points: left, bottom */
	unsigned page_size[2];    /* points */
	unsigned width, height;   /* pixels */
	unsigned media_type;
	unsigned bits_per_color, bits_per_pixel, bytes_per_line;
	unsigned color_order, color_space;
	unsigned num_copies;
	unsigned integer[16];
	char page_size_name[64];
};

struct raster;
struct raster *raster_open(int fd);
bool raster_next_page(struct raster *r, struct raster_page *pg);
/* reads one line, converted to 1 bpp, 1 = black, `out` gets bytes_per_line1 bytes */
bool raster_read_line(struct raster *r, uint8_t *out);
unsigned raster_line_bytes_1bpp(const struct raster *r);
void raster_close(struct raster *r);

/* ---------- Hi-SCoA compression (hiscoa.c) ---------- */
size_t hiscoa_compress_band(const uint8_t *band, unsigned line_size, unsigned nlines,
                            bool last, uint8_t *out, size_t out_size);
size_t hiscoa_format_params(uint8_t *buf, size_t size);

/* ---------- CAPT protocol (capt.c) ---------- */
enum capt_cmd {
	CAPT_CHKJOBSTAT  = 0xA0A1,
	CAPT_CHKXSTATUS  = 0xA0A8,
	CAPT_IDENT       = 0xA1A1,
	CAPT_JOB_BEGIN   = 0xA2A0,
	CAPT_START_0     = 0xA3A2,
	CAPT_PRINT_DATA  = 0xC0A0,
	CAPT_PRINT_DATA_END = 0xC0A4,
	CAPT_SET_PARM_PAGE  = 0xD0A0,
	CAPT_SET_PARM_1     = 0xD0A1,
	CAPT_SET_PARM_2     = 0xD0A2,
	CAPT_SET_PARM_HISCOA = 0xD0A4,
	CAPT_SET_PARMS   = 0xD0A9,
	CAPT_CHKSTATUS   = 0xE0A0,
	CAPT_START_2     = 0xE0A2,
	CAPT_START_1     = 0xE0A3,
	CAPT_START_3     = 0xE0A4,
	CAPT_UPLOAD_2    = 0xE0A5,  /* GoOnline */
	CAPT_GO_OFFLINE  = 0xE0A6,
	CAPT_FIRE        = 0xE0A7,
	CAPT_JOB_END     = 0xE0A9,
	CAPT_JOB_SETUP   = 0xE1A1,
	CAPT_GPIO        = 0xE1A2,
};

struct capt_status {
	uint16_t s[7];
	uint16_t page_decoding, page_printing, page_out, page_completed, page_received;
	uint8_t buflevel;      /* free FIFO slots 0..15 (0 = buffer full) */
	uint8_t aux, cnt, pap; /* extended status: engine aux, comm flags, paper availability */
	uint8_t input_flags;   /* GetInputStatus byte 3: 0xC0/0xD0 ready, 0x80 paper-out error */
	uint8_t input_paper;   /* GetInputStatus byte 6: paper size code the printer assumes */
};

/* status flag helpers. s[0] = basic status byte 1 | byte 2 << 8 */
#define ST_B1(st)          ((st)->s[0] & 0xFF)
#define ST_B2(st)          (((st)->s[0] >> 8) & 0xFF)
#define ST_BUSY(st)        (((st)->s[0] >> 7) & 1)
#define ST_XSTATUS_CHG(st) (ST_B2(st) & 0x01)
#define ST_NEED_INPUT(st)  (ST_B2(st) & 0x02)
#define ST_UNINIT(st)      ((((st)->s[0] >> 5) & 1) | (((st)->s[0] >> 4) & 1))
#define ST_BUFFERFULL(st)  (((st)->s[0] >> 2) & 1)
#define ST_PRINTING(st)    ((st)->aux & 0x04)   /* paper delivery in progress */
#define ST_PROCESSING1(st) ((st)->aux & 0x80)   /* motor running */
#define ST_BUTTON(st)      (((st)->s[1] >> 5) & 1)
/* paper out: print rejected, or not-ready+offline with the engine reporting no paper */
#define ST_NOPAPER(st)     (((st)->cnt & 0x40) || ((ST_B1(st) & 0x12) == 0x12 && (st)->pap == 0))

struct page_params {
	unsigned line_size;    /* bytes per band line */
	unsigned num_lines;
	unsigned paper_width, paper_height;  /* pixels */
	unsigned margin_h, margin_w;         /* first and second margin field */
	uint8_t  size_code;
	uint8_t  media_type;
	uint8_t  media_adapt;
	uint8_t  toner_save;
};

/* experimental switches, set per job with lp -o capt-...=... */
struct capt_opts {
	bool legacy_poll;   /* capt-poll=legacy: old captdriver polling (every 16 chunks) */
};
extern struct capt_opts g_opts;

int capt_job_begin(void);
int capt_page_begin(const struct page_params *pp);
/* these return 0 ok, 1 = page lost (out of paper), -1 = error */
int capt_send_band(const uint8_t *data, size_t size, unsigned *chunk_counter);
int capt_page_end(void);
int capt_recover_paper(void);
int capt_job_end(void);
void capt_job_abort(void);
