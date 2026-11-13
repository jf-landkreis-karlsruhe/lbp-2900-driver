/*
 * Hi-SCoA band compression as used by Canon CAPT printers (LBP2900 & co).
 *
 * The stream is an MSB-first bit stream of these codes:
 *
 *   literal byte 0x00          11111101
 *   literal byte b != 0        1101 bbbbbbbb
 *   recently-used byte i       10 iiii          (i = 15 - index in MRU list)
 *   long-length prefix         11111100 nn v..  (multiples of 128)
 *   copy from distance origin[c], encoded as c ones + zero (c=2 adds a 0 bit),
 *     followed by a length code:
 *       0 -> 111111, 1 -> 00, 2 -> 011, 3 -> 010,
 *       4.. -> (k-1 ones, 0), ~len (k bits), k = bitlength(len) - 1
 *   end of band                11111110 tt  then 1-bits up to a 32-bit boundary
 *                              (tt = 01 marks the last band of a page, else 00)
 *
 * Every output byte is finally XORed with 0x43.
 * Copy distances ("origins") are: one line up, previous byte, 4 bytes back and
 * one line up minus 7; using origin 2 or 5 swaps it with origin 0 or 3.
 */
#include "capt.h"

#include <string.h>

#define MAX_MATCH (512 + 127)

static const int ORIGIN_3 = 1, ORIGIN_5 = 4, ORIGIN_0 = 0, ORIGIN_2 = -7, ORIGIN_4 = 0;

struct bitwriter {
	uint8_t *buf;
	size_t cap_bits;
	size_t pos;
};

/* append the low `n` bits of `bits`, most significant first */
static void put(struct bitwriter *w, uint32_t bits, unsigned n)
{
	while (n--) {
		if (w->pos >= w->cap_bits)
			return;
		if ((bits >> n) & 1)
			w->buf[w->pos >> 3] |= (uint8_t) (0x80 >> (w->pos & 7));
		w->pos++;
	}
}

static unsigned bitlen(unsigned v)
{
	unsigned n = 0;
	while (v) {
		n++;
		v >>= 1;
	}
	return n;
}

static unsigned match_len(const uint8_t *in, size_t total, unsigned line_size, size_t pos, unsigned dist)
{
	if (pos < dist)
		return 0;
	size_t p = pos;
	while (in[p] == in[p - dist]) {
		p++;
		if (p >= total || p >= pos + MAX_MATCH || p % line_size == 0)
			break;
	}
	return (unsigned) (p - pos);
}

static void put_copy(struct bitwriter *w, unsigned cmd, unsigned len)
{
	if (len > 127) {
		unsigned val = len / 128;
		unsigned nb = bitlen(val) - 1;
		put(w, 0xFC, 8);
		put(w, nb, 2);
		put(w, ~val, nb);
		len %= 128;
	}
	put(w, 0xFFFFFFFE, cmd + 1);
	if (cmd == 2)
		put(w, 0, 1);
	switch (len) {
	case 0: put(w, 0x3F, 6); break;
	case 1: put(w, 0x0, 2); break;
	case 2: put(w, 0x3, 3); break;
	case 3: put(w, 0x2, 3); break;
	default: {
		unsigned nb = bitlen(len) - 1;
		put(w, 0xFFFFFFFE, nb);
		put(w, ~len, nb);
	}
	}
}

size_t hiscoa_compress_band(const uint8_t *in, unsigned line_size, unsigned nlines,
                            bool last, uint8_t *out, size_t out_size)
{
	struct bitwriter w = { out, out_size * 8, 0 };
	size_t total = (size_t) line_size * nlines;
	size_t pos = 0;
	unsigned origin[6];
	uint8_t mru[16];
	unsigned nmru = 0;

	memset(out, 0, out_size);
	origin[0] = (unsigned) ((int) line_size + ORIGIN_0);
	origin[1] = 0;
	origin[2] = (unsigned) ((int) line_size + ORIGIN_2);
	origin[3] = (unsigned) ORIGIN_3;
	origin[4] = (unsigned) ORIGIN_4;
	origin[5] = (unsigned) ORIGIN_5;

	while (pos < total) {
		/* 1. back-reference */
		unsigned best_len = 0, best_cmd = 0;
		for (unsigned c = 0; c < 6; c++) {
			if (!origin[c])
				continue;
			unsigned l = match_len(in, total, line_size, pos, origin[c]);
			if (l > best_len) {
				best_len = l;
				best_cmd = c;
			}
		}
		if (best_len > 1) {
			put_copy(&w, best_cmd, best_len);
			if (best_cmd == 2) {
				unsigned t = origin[2]; origin[2] = origin[0]; origin[0] = t;
			} else if (best_cmd == 5) {
				unsigned t = origin[5]; origin[5] = origin[3]; origin[3] = t;
			}
			pos += best_len;
			continue;
		}

		/* 2. recently used byte */
		uint8_t b = in[pos];
		unsigned i;
		for (i = 0; i < nmru; i++)
			if (mru[i] == b)
				break;
		if (i < nmru) {
			memmove(mru + 1, mru, i);
			mru[0] = b;
			put(&w, 0x20 | ((15 - i) & 0xF), 6);
			pos++;
			continue;
		}

		/* 3. literal */
		if (nmru < 16)
			nmru++;
		memmove(mru + 1, mru, nmru - 1);
		mru[0] = b;
		if (b == 0)
			put(&w, 0xFD, 8);
		else
			put(&w, 0xD00 | b, 12);
		pos++;
	}

	put(&w, 0xFE, 8);   /* end of band */
	put(&w, last ? 1 : 0, 2);  /* end-of-band type: 1 = last band of the page */
	put(&w, 0xFFFFFFFF, 32 - (unsigned) (w.pos % 32));

	size_t nbytes = w.pos / 8;
	for (size_t k = 0; k < nbytes; k++)
		out[k] ^= 0x43;
	return nbytes;
}

size_t hiscoa_format_params(uint8_t *buf, size_t size)
{
	const uint8_t b[8] = {
		(uint8_t) (int8_t) ORIGIN_3, (uint8_t) (int8_t) ORIGIN_5, 0x01, 0x01,
		(uint8_t) (int8_t) ORIGIN_0, (uint8_t) (int8_t) ORIGIN_2,
		LO((uint16_t) (int16_t) ORIGIN_4), HI((uint16_t) (int16_t) ORIGIN_4),
	};
	if (size < sizeof(b))
		return 0;
	memcpy(buf, b, sizeof(b));
	return sizeof(b);
}
