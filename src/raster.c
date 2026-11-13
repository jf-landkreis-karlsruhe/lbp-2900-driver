/*
 * Minimal CUPS / PWG raster reader (no libcups needed).
 *
 * Supports sync words RaSt/RaS2/RaS3 in both byte orders (v2 = compressed),
 * 1-bit and 8-bit grayscale (K, W, SW colorspaces). Output is always 1 bpp
 * with 1 = black; 8-bit input is ordered-dithered.
 */
#include "capt.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HEADER_SIZE 1796

enum { CS_W = 0, CS_K = 3, CS_SW = 18 };

struct raster {
	int fd;
	bool big_endian;
	bool compressed;
	uint8_t inbuf[65536];
	size_t in_pos, in_len;
	bool eof;

	struct raster_page pg;
	unsigned bpp_unit;        /* bytes per pixel unit for v2 compression */
	unsigned lines_left;
	unsigned line_no;
	unsigned repeat_left;     /* v2: remaining repeats of current line */
	uint8_t *line;            /* decoded raw line */
	bool invert;
	bool dither;
};

static const uint8_t bayer8[8][8] = {
	{  0, 32,  8, 40,  2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
	{ 12, 44,  4, 36, 14, 46,  6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
	{  3, 35, 11, 43,  1, 33,  9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
	{ 15, 47,  7, 39, 13, 45,  5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
};

static bool rd(struct raster *r, void *dst, size_t len)
{
	uint8_t *d = dst;
	while (len) {
		if (r->in_pos >= r->in_len) {
			if (r->eof)
				return false;
			ssize_t n = read(r->fd, r->inbuf, sizeof(r->inbuf));
			if (n < 0 && errno == EINTR)
				continue;
			if (n <= 0) {
				r->eof = true;
				return false;
			}
			r->in_pos = 0;
			r->in_len = (size_t) n;
		}
		size_t c = r->in_len - r->in_pos;
		if (c > len)
			c = len;
		memcpy(d, r->inbuf + r->in_pos, c);
		r->in_pos += c;
		d += c;
		len -= c;
	}
	return true;
}

static unsigned u32(const struct raster *r, const uint8_t *h, size_t off)
{
	const uint8_t *p = h + off;
	if (r->big_endian)
		return (unsigned) p[0] << 24 | (unsigned) p[1] << 16 | (unsigned) p[2] << 8 | p[3];
	return (unsigned) p[3] << 24 | (unsigned) p[2] << 16 | (unsigned) p[1] << 8 | p[0];
}

struct raster *raster_open(int fd)
{
	struct raster *r = calloc(1, sizeof(*r));
	if (!r)
		return NULL;
	r->fd = fd;
	uint8_t sync[4];
	if (!rd(r, sync, 4)) {
		ERR("CAPT: empty job (no raster data)");
		free(r);
		return NULL;
	}
	char v;
	if (sync[0] == 'R' && sync[1] == 'a' && sync[2] == 'S') {
		r->big_endian = true;
		v = (char) sync[3];
	} else if (sync[3] == 'R' && sync[2] == 'a' && sync[1] == 'S') {
		r->big_endian = false;
		v = (char) sync[0];
	} else {
		ERR("CAPT: input is not CUPS raster (starts with %02X %02X %02X %02X)",
		    sync[0], sync[1], sync[2], sync[3]);
		free(r);
		return NULL;
	}
	if (v == '2')
		r->compressed = true;
	else if (v != 't' && v != '3') {
		ERR("CAPT: unsupported raster version '%c'", v);
		free(r);
		return NULL;
	}
	DBG("CAPT: raster stream: %s-endian, %s", r->big_endian ? "big" : "little",
	    r->compressed ? "compressed" : "uncompressed");
	return r;
}

bool raster_next_page(struct raster *r, struct raster_page *pg)
{
	/* skip any unread lines of the previous page */
	if (r->line) {
		uint8_t *tmp = malloc(raster_line_bytes_1bpp(r) + 1);
		while (r->lines_left && tmp && raster_read_line(r, tmp))
			;
		free(tmp);
		free(r->line);
		r->line = NULL;
	}

	uint8_t h[HEADER_SIZE];
	if (!rd(r, h, sizeof(h)))
		return false;

	memset(pg, 0, sizeof(*pg));
	pg->hwres[0] = u32(r, h, 276);
	pg->hwres[1] = u32(r, h, 280);
	pg->margins[0] = u32(r, h, 312);
	pg->margins[1] = u32(r, h, 316);
	pg->num_copies = u32(r, h, 340);
	pg->page_size[0] = u32(r, h, 352);
	pg->page_size[1] = u32(r, h, 356);
	pg->width = u32(r, h, 372);
	pg->height = u32(r, h, 376);
	pg->media_type = u32(r, h, 380);
	pg->bits_per_color = u32(r, h, 384);
	pg->bits_per_pixel = u32(r, h, 388);
	pg->bytes_per_line = u32(r, h, 392);
	pg->color_order = u32(r, h, 396);
	pg->color_space = u32(r, h, 400);
	for (int i = 0; i < 16; i++)
		pg->integer[i] = u32(r, h, 452 + 4 * i);
	memcpy(pg->page_size_name, h + 1732, 63);
	pg->page_size_name[63] = '\0';

	DBG("CAPT: page %ux%u px, %u bpp, %u bytes/line, cs %u, %ux%u dpi, page %ux%u pt, margins %u/%u, size '%s'",
	    pg->width, pg->height, pg->bits_per_pixel, pg->bytes_per_line, pg->color_space,
	    pg->hwres[0], pg->hwres[1], pg->page_size[0], pg->page_size[1],
	    pg->margins[0], pg->margins[1], pg->page_size_name);

	if (pg->width == 0 || pg->height == 0 || pg->width > 20000 || pg->height > 30000 ||
	    pg->bytes_per_line == 0 || pg->bytes_per_line > 65536) {
		ERR("CAPT: invalid raster page header");
		return false;
	}
	if (!((pg->bits_per_pixel == 1 || pg->bits_per_pixel == 8) && pg->bits_per_color == pg->bits_per_pixel)) {
		ERR("CAPT: unsupported raster format (%u bits/pixel); expected 1-bit or 8-bit grayscale",
		    pg->bits_per_pixel);
		return false;
	}
	if (pg->color_space != CS_K && pg->color_space != CS_W && pg->color_space != CS_SW)
		WARN("CAPT: unexpected color space %u, treating as grayscale", pg->color_space);
	if (pg->hwres[0] != 600 || pg->hwres[1] != 600)
		WARN("CAPT: raster resolution is %ux%u, printer expects 600x600", pg->hwres[0], pg->hwres[1]);

	r->pg = *pg;
	r->bpp_unit = (pg->bits_per_pixel + 7) / 8;
	r->lines_left = pg->height;
	r->line_no = 0;
	r->repeat_left = 0;
	r->dither = pg->bits_per_pixel == 8;
	/* K: 1/255 = black. W/SW: 0 = black. */
	r->invert = pg->color_space != CS_K;
	r->line = calloc(1, pg->bytes_per_line);
	return r->line != NULL;
}

unsigned raster_line_bytes_1bpp(const struct raster *r)
{
	return (r->pg.width + 7) / 8;
}

static bool decode_line(struct raster *r)
{
	unsigned bpl = r->pg.bytes_per_line;
	if (!r->compressed)
		return rd(r, r->line, bpl);

	if (r->repeat_left) {
		r->repeat_left--;
		return true;
	}
	uint8_t rep;
	if (!rd(r, &rep, 1))
		return false;
	r->repeat_left = rep;
	unsigned pos = 0, unit = r->bpp_unit;
	while (pos < bpl) {
		uint8_t c;
		if (!rd(r, &c, 1))
			return false;
		if (c & 0x80) {
			unsigned count = (257u - c) * unit;
			if (count > bpl - pos)
				count = bpl - pos;
			if (!rd(r, r->line + pos, count))
				return false;
			pos += count;
		} else {
			unsigned count = (c + 1u) * unit;
			uint8_t px[8];
			if (!rd(r, px, unit))
				return false;
			if (count > bpl - pos)
				count = bpl - pos;
			for (unsigned i = 0; i < count; i++)
				r->line[pos + i] = px[i % unit];
			pos += count;
		}
	}
	return true;
}

bool raster_read_line(struct raster *r, uint8_t *out)
{
	if (!r->lines_left)
		return false;
	if (!decode_line(r))
		return false;
	r->lines_left--;

	unsigned w = r->pg.width;
	unsigned nb = (w + 7) / 8;
	if (!r->dither) {
		memcpy(out, r->line, nb);
		if (r->invert)
			for (unsigned i = 0; i < nb; i++)
				out[i] = (uint8_t) ~out[i];
	} else {
		memset(out, 0, nb);
		const uint8_t *row = bayer8[r->line_no & 7];
		for (unsigned x = 0; x < w; x++) {
			unsigned v = r->line[x];          /* K: 255 = black */
			if (r->invert)
				v = 255 - v;
			if (v * 64 / 256 > row[x & 7])
				out[x >> 3] |= (uint8_t) (0x80 >> (x & 7));
		}
	}
	/* clear padding bits beyond width */
	if (w & 7)
		out[nb - 1] &= (uint8_t) (0xFF << (8 - (w & 7)));
	r->line_no++;
	return true;
}

void raster_close(struct raster *r)
{
	if (!r)
		return;
	free(r->line);
	free(r);
}
