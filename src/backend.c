/*
 * capt - CUPS backend for Canon CAPT printers (LBP2900).
 *
 *   capt                         list attached printers (CUPS device discovery)
 *   capt job user title copies options [file]
 *                                print a CUPS raster job; device from $DEVICE_URI
 *
 * The PPD makes CUPS render documents to 600 dpi 1-bit raster and pass it
 * straight to this backend, which owns the bidirectional CAPT conversation.
 */
#include "capt.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CONF_FILE "/etc/capt-lbp.conf"
#define DEFAULT_LOG "/var/log/capt-lbp.log"
#define BAND_LINES 70

enum { BACKEND_OK = 0, BACKEND_FAILED = 1, BACKEND_STOP = 4 };

volatile int g_cancelled;

static void on_term(int sig)
{
	(void) sig;
	g_cancelled = 1;
}

/* --------------------------------------------------------------- config */

static char conf_log[600] = DEFAULT_LOG;

static void read_config(void)
{
	FILE *f = fopen(CONF_FILE, "r");
	if (!f)
		return;
	char line[600];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "log=", 4))
			snprintf(conf_log, sizeof(conf_log), "%s", line + 4);
	}
	fclose(f);
}

/* ---------------------------------------------------------- job options */

/* experimental switches, e.g. lp -o capt-sizecode=a4 -o capt-geometry=canon */
static bool opt_sizecode_a4;     /* always send the A4 size code (captdriver behaviour) */
static bool opt_geometry_canon;  /* page geometry as computed by Canon's driver */

static const char *find_opt(const char *opts, const char *key)
{
	size_t kl = strlen(key);
	for (const char *p = opts; p && *p;) {
		while (*p == ' ')
			p++;
		if (!strncmp(p, key, kl) && p[kl] == '=')
			return p + kl + 1;
		p = strchr(p, ' ');
	}
	return NULL;
}

static bool opt_is(const char *opts, const char *key, const char *val)
{
	const char *v = find_opt(opts, key);
	size_t vl = strlen(val);
	return v && !strncmp(v, val, vl) && (v[vl] == '\0' || v[vl] == ' ');
}

static void parse_options(const char *opts)
{
	opt_sizecode_a4 = opt_is(opts, "capt-sizecode", "a4");
	opt_geometry_canon = opt_is(opts, "capt-geometry", "canon");
	g_opts.legacy_poll = opt_is(opts, "capt-poll", "legacy");
	if (opt_sizecode_a4 || opt_geometry_canon || g_opts.legacy_poll)
		INFO("CAPT: options: sizecode=%s geometry=%s poll=%s", opt_sizecode_a4 ? "a4" : "auto",
		     opt_geometry_canon ? "canon" : "captdriver", g_opts.legacy_poll ? "legacy" : "flow");
}

/* ------------------------------------------------------------ discovery */

static void url_encode(const char *in, char *out, size_t size)
{
	size_t o = 0;
	for (; *in && o + 4 < size; in++) {
		unsigned char c = (unsigned char) *in;
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
		    c == '-' || c == '_' || c == '.')
			out[o++] = (char) c;
		else
			o += (size_t) snprintf(out + o, size - o, "%%%02X", c);
	}
	out[o] = '\0';
}

static int discover(void)
{
	struct capt_device devs[16];
	int n = dev_enumerate(devs, ARRAY_SIZE(devs));
	for (int i = 0; i < n; i++) {
		char m[200], s[200];
		url_encode(devs[i].model, m, sizeof(m));
		url_encode(devs[i].serial, s, sizeof(s));
		printf("direct capt://Canon/%s%s%s \"Canon %s\" \"Canon %s (CAPT)\" \"%s\" \"\"\n",
		       m, s[0] ? "?serial=" : "", s, devs[i].model, devs[i].model, devs[i].ieee1284);
	}
	if (n == 0)
		printf("direct capt \"Unknown\" \"Canon CAPT printer (USB)\"\n");
	return 0;
}

/* ---------------------------------------------------------- page layout */

struct size_code { unsigned w, h; uint8_t code; const char *name; };
static const struct size_code sizes[] = {
	{ 595, 842, 0x02, "A4" },     { 420, 595, 0x03, "A5" },
	{ 516, 729, 0x07, "B5" },     { 522, 756, 0x0A, "Executive" },
	{ 612, 1008, 0x0C, "Legal" }, { 612, 792, 0x0D, "Letter" },
	{ 459, 649, 0x15, "EnvC5" },  { 297, 684, 0x16, "Env10" },
	{ 279, 540, 0x17, "EnvMonarch" }, { 312, 624, 0x18, "EnvDL" },
};

static uint8_t size_code_for(unsigned w, unsigned h)
{
	for (size_t i = 0; i < ARRAY_SIZE(sizes); i++) {
		int dw = (int) w - (int) sizes[i].w, dh = (int) h - (int) sizes[i].h;
		if (dw >= -2 && dw <= 2 && dh >= -2 && dh <= 2) {
			DBG("CAPT: paper size %s (code 0x%02X)", sizes[i].name, sizes[i].code);
			return sizes[i].code;
		}
	}
	DBG("CAPT: custom paper size %ux%u pt, using A4 code", w, h);
	return 0x02;
}

/* --------------------------------------------------------- page caching */

struct band { uint8_t *data; size_t size; };
struct page {
	struct page_params pp;
	struct band *bands;
	unsigned nbands;
};

static void page_free(struct page *p)
{
	for (unsigned i = 0; i < p->nbands; i++)
		free(p->bands[i].data);
	free(p->bands);
	memset(p, 0, sizeof(*p));
}

/* read one raster page and compress it into bands */
static int page_load(struct raster *r, const struct raster_page *rp, struct page *p)
{
	memset(p, 0, sizeof(*p));
	unsigned src_bytes = raster_line_bytes_1bpp(r);
	unsigned ls, copy, dst_off, src_off;

	p->pp.num_lines = rp->height;
	p->pp.size_code = opt_sizecode_a4 ? 0x02 : size_code_for(rp->page_size[0], rp->page_size[1]);
	p->pp.media_type = rp->media_type <= 4 ? (uint8_t) rp->media_type : 0;
	p->pp.media_adapt = rp->hwres[1] == 400 ? 0x81 : 0x11;
	p->pp.toner_save = rp->integer[0] ? 1 : 0;

	if (opt_geometry_canon) {
		/* Canon driver: line padded to 32 pixels, physical paper size and
		 * margins (left, top) in device pixels */
		unsigned xres = rp->hwres[0] ? rp->hwres[0] : 600, yres = rp->hwres[1] ? rp->hwres[1] : 600;
		ls = ((rp->width + 31) & ~31u) / 8;
		copy = src_bytes;
		dst_off = src_off = 0;
		p->pp.paper_width = (rp->page_size[0] * xres + 36) / 72;
		p->pp.paper_height = (rp->page_size[1] * yres + 36) / 72;
		p->pp.margin_h = (rp->margins[0] * xres + 36) / 72;
		int top = (int) p->pp.paper_height - (int) ((rp->margins[1] * yres + 36) / 72) - (int) rp->height;
		p->pp.margin_w = top > 0 ? (unsigned) top : 0;
	} else {
		/* captdriver: line size = page width in points (as bytes), raster centered,
		 * printable size in pixels, margins in points */
		ls = rp->page_size[0];
		if (ls < src_bytes / 2 || ls > 4096)
			ls = src_bytes;
		copy = src_bytes < ls ? src_bytes : ls;
		dst_off = src_bytes < ls ? (ls - src_bytes) / 2 : 0;
		src_off = src_bytes > ls ? (src_bytes - ls) / 2 : 0;
		p->pp.paper_width = rp->width;
		p->pp.paper_height = rp->height;
		p->pp.margin_h = rp->margins[0];
		p->pp.margin_w = rp->margins[1];
	}
	p->pp.line_size = ls;
	DBG("CAPT: geometry: line %u bytes, %u lines, paper %ux%u, margins %u/%u, size code 0x%02X",
	    ls, p->pp.num_lines, p->pp.paper_width, p->pp.paper_height, p->pp.margin_h, p->pp.margin_w,
	    p->pp.size_code);

	unsigned nb = (rp->height + BAND_LINES - 1) / BAND_LINES;
	p->bands = calloc(nb, sizeof(*p->bands));
	uint8_t *line = malloc(src_bytes + 8);
	uint8_t *band = malloc((size_t) ls * BAND_LINES);
	size_t comp_cap = 2 * (size_t) ls * BAND_LINES + 64;
	uint8_t *comp = malloc(comp_cap);
	if (!p->bands || !line || !band || !comp) {
		ERR("CAPT: out of memory");
		free(line); free(band); free(comp);
		return -1;
	}

	size_t total = 0;
	for (unsigned b = 0; b < nb; b++) {
		unsigned start = b * BAND_LINES;
		unsigned nl = rp->height - start < BAND_LINES ? rp->height - start : BAND_LINES;
		memset(band, 0, (size_t) ls * nl);
		for (unsigned l = 0; l < nl; l++) {
			if (!raster_read_line(r, line)) {
				ERR("CAPT: raster data ends early (line %u)", start + l);
				free(line); free(band); free(comp);
				return -1;
			}
			memcpy(band + (size_t) l * ls + dst_off, line + src_off, copy);
		}
		size_t cs = hiscoa_compress_band(band, ls, nl, b == nb - 1, comp, comp_cap);
		p->bands[b].data = malloc(cs ? cs : 1);
		if (!p->bands[b].data) {
			free(line); free(band); free(comp);
			return -1;
		}
		memcpy(p->bands[b].data, comp, cs);
		p->bands[b].size = cs;
		p->nbands = b + 1;
		total += cs;
	}
	free(line); free(band); free(comp);
	DBG("CAPT: page compressed: %u bands, %zu bytes", p->nbands, total);
	return 0;
}

/* --------------------------------------------------------------- job */

static int open_device(const char *uri)
{
	if (!strncmp(uri, "capt-sim:", 9))
		return tr_open_uri_special(uri);

	struct capt_device dev;
	bool waiting = false;
	while (dev_find(uri, &dev) < 0) {
		if (g_cancelled)
			return -1;
		if (!waiting) {
			INFO("Waiting for printer to become available (%s)", uri);
			cups_state("+connecting-to-device");
			waiting = true;
		}
		sleep(5);
	}
	if (waiting)
		cups_state("-connecting-to-device");
	DBG("CAPT: found %s serial %s (%s) at %s", dev.model, dev.serial, dev.ieee1284,
	    dev.lp_node[0] ? dev.lp_node : "usbfs");
	if (dev.model[0] && strcmp(dev.model, "LBP2900") && strcmp(dev.model, "LBP3000"))
		WARN("CAPT: model %s is untested with this driver", dev.model);
	return tr_open(&dev);
}

static int print_stream(int fd, bool *job_open, unsigned *page_no)
{
	struct raster *r = raster_open(fd);
	if (!r)
		return -1;

	struct raster_page rp;
	struct page pg;
	int rc = 0;

	while (!g_cancelled && raster_next_page(r, &rp)) {
		(*page_no)++;
		INFO("Rendering page %u", *page_no);
		if (page_load(r, &rp, &pg) < 0) {
			page_free(&pg);
			rc = -1;
			break;
		}
		if (g_cancelled) {
			page_free(&pg);
			break;
		}
		if (!*job_open) {
			INFO("Starting print job");
			if (capt_job_begin() < 0) {
				page_free(&pg);
				rc = -1;
				break;
			}
			*job_open = true;
		}

		size_t total = 0;
		for (unsigned b = 0; b < pg.nbands; b++)
			total += pg.bands[b].size;

		for (;;) {
			INFO("Printing page %u", *page_no);
			if (capt_page_begin(&pg.pp) < 0) {
				rc = -1;
				break;
			}
			unsigned chunks = 0, last_pct = 0;
			size_t sent = 0;
			int e = 0;
			for (unsigned b = 0; b < pg.nbands && e == 0; b++) {
				e = capt_send_band(pg.bands[b].data, pg.bands[b].size, &chunks);
				sent += pg.bands[b].size;
				unsigned pct = total ? (unsigned) (sent * 100 / total) : 100;
				if (pct / 25 != last_pct / 25) {
					INFO("Printing page %u (%u%% sent)", *page_no, pct);
					last_pct = pct;
				}
			}
			if (e == 0)
				e = capt_page_end();
			if (e < 0) {
				rc = -1;
				break;
			}
			if (e == 0)
				break;
			/* out of paper: page was not printed, wait for the user and retry */
			cups_state("+media-empty-error");
			INFO("Out of paper - load paper and press the printer's button");
			int w = capt_recover_paper();
			cups_state("-media-empty-error");
			if (w < 0) {
				rc = -1;
				break;
			}
		}
		page_free(&pg);
		if (rc)
			break;
		fprintf(stderr, "PAGE: %u 1\n", *page_no);
	}
	raster_close(r);
	return rc;
}

int main(int argc, char *argv[])
{
	setvbuf(stderr, NULL, _IOLBF, 0);
	signal(SIGPIPE, SIG_IGN);
	struct sigaction sa = { .sa_handler = on_term };
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);

	if (argc == 1)
		return discover();

	if (argc < 6 || argc > 7) {
		fprintf(stderr, "Usage: %s job-id user title copies options [file]\n", argv[0]);
		return BACKEND_FAILED;
	}

	read_config();
	parse_options(argv[5]);
	log_open_file(getenv("CAPT_LOG") ? getenv("CAPT_LOG") : conf_log);

	const char *uri = getenv("DEVICE_URI");
	if (!uri && strchr(argv[0], ':'))
		uri = argv[0];
	if (!uri) {
		ERR("CAPT: DEVICE_URI not set");
		return BACKEND_FAILED;
	}
	DBG("CAPT: job %s user %s title '%s' uri %s", argv[1], argv[2], argv[3], uri);

	int copies = argc == 7 ? atoi(argv[4]) : 1;
	if (copies < 1)
		copies = 1;

	if (open_device(uri) < 0) {
		if (g_cancelled)
			return BACKEND_OK;
		ERR("CAPT: cannot open printer");
		return BACKEND_FAILED;
	}

	bool job_open = false;
	unsigned page_no = 0;
	int rc = 0;
	for (int c = 0; c < copies && rc == 0 && !g_cancelled; c++) {
		int fd = 0;
		if (argc == 7) {
			fd = open(argv[6], O_RDONLY | O_CLOEXEC);
			if (fd < 0) {
				ERR("CAPT: cannot open %s: %s", argv[6], strerror(errno));
				rc = -1;
				break;
			}
		}
		rc = print_stream(fd, &job_open, &page_no);
		if (fd > 0)
			close(fd);
	}

	if (job_open) {
		if (g_cancelled || rc) {
			INFO("Aborting print job");
			capt_job_abort();
		} else {
			INFO("Finishing print job");
			if (capt_job_end() < 0)
				rc = -1;
		}
	}
	tr_close();

	if (g_cancelled) {
		INFO("Job cancelled");
		return BACKEND_OK;
	}
	if (rc) {
		ERR("CAPT: print job failed (details in the backend log)");
		return BACKEND_FAILED;
	}
	if (page_no == 0)
		WARN("CAPT: job contained no pages");
	INFO("Ready to print.");
	return BACKEND_OK;
}
