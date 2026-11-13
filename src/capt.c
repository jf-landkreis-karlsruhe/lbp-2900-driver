/*
 * CAPT protocol: packet framing, status polling and the job/page sequence
 * for the LBP2900 family.
 *
 * Packet: cmd (u16 LE), total length incl. 4-byte header (u16 LE), payload.
 * Most commands are answered by the printer with a packet carrying the same
 * command code; print data (C0A0), end of data (C0A4) and the parameter
 * container (D0A9) are not answered.
 *
 * Status handling follows the observed behaviour of Canon's own drivers:
 * the basic status (E0A0) is polled after every data chunk; when it flags
 * "extended status changed" the extended status (A0A8) is read, and when it
 * flags "input status changed" the input status (A0A1) must be read - the
 * printer stalls otherwise. The basic status also reports the number of free
 * FIFO slots (BufLevel); when it reaches 0 while data remains, printing is
 * started early ("streaming mode") so the engine drains the buffer.
 */
#include "capt.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define REPLY_TIMEOUT_MS 15000
#define DATA_CHUNK 0xFF00

struct capt_opts g_opts;

static uint8_t txbuf[0x10000 + 64];
static uint8_t rxpkt[0x10000 + 64];
static struct capt_status st;
static uint16_t job_id;
static uint16_t last_out;     /* page_out counter after the last printed page */
static bool page_fired;       /* StartPrint already sent for the current page (streaming) */
static uint16_t fired_page;
static uint8_t last_basic[16];
static int last_basic_len = -1;

static uint16_t bcd16(const uint8_t *p)
{
	unsigned d[4] = { (unsigned) p[1] >> 4, (unsigned) p[1] & 15, (unsigned) p[0] >> 4, (unsigned) p[0] & 15 };
	for (int i = 0; i < 4; i++)
		if (d[i] > 9)
			return WORD(p);
	return (uint16_t) (d[0] * 1000 + d[1] * 100 + d[2] * 10 + d[3]);
}

static size_t frame(uint8_t *buf, uint16_t cmd, const void *data, size_t len)
{
	buf[0] = LO(cmd);
	buf[1] = HI(cmd);
	buf[2] = LO((uint16_t) (len + 4));
	buf[3] = HI((uint16_t) (len + 4));
	if (len)
		memcpy(buf + 4, data, len);
	return len + 4;
}

static int capt_send(uint16_t cmd, const void *data, size_t len)
{
	size_t n = frame(txbuf, cmd, data, len);
	if (cmd != CAPT_PRINT_DATA && cmd != CAPT_CHKSTATUS)
		log_hex("CAPT: send", txbuf, n, 48);
	return tr_write(txbuf, n);
}

/* read one complete reply packet into rxpkt; returns total length or -1 */
static int recv_packet(uint16_t cmd)
{
	size_t got = 0;
	for (;;) {
		ssize_t n = tr_read(rxpkt + got, sizeof(rxpkt) - got, REPLY_TIMEOUT_MS);
		if (n < 0) {
			ERR("CAPT: read from printer failed");
			return -1;
		}
		if (n == 0) {
			ERR("CAPT: no reply from printer to command %04X", cmd);
			return -1;
		}
		got += (size_t) n;
		if (got < 4)
			continue;
		size_t w = WORD(rxpkt + 2), b = bcd16(rxpkt + 2);
		if (got == w || got == b)
			return (int) got;
		if (got > w && got > b) {
			log_hex("CAPT: oversized reply:", rxpkt, got, 32);
			return (int) got;
		}
	}
}

/* Send a command and wait for the matching reply. Returns payload length or -1. */
static int capt_sendrecv(uint16_t cmd, const void *data, size_t len, const uint8_t **payload)
{
	if (capt_send(cmd, data, len) < 0)
		return -1;

	for (int tries = 0; tries < 4; tries++) {
		int got = recv_packet(cmd);
		if (got < 0)
			return -1;
		if (WORD(rxpkt) != cmd) {
			log_hex("CAPT: ignoring unexpected packet:", rxpkt, (size_t) got, 32);
			continue;
		}
		if (cmd == CAPT_CHKSTATUS) {
			/* basic status is polled constantly: only log changes */
			if (got != last_basic_len || memcmp(last_basic, rxpkt, (size_t) got < sizeof(last_basic) ? (size_t) got : sizeof(last_basic))) {
				log_hex("CAPT: basic status", rxpkt, (size_t) got, 16);
				last_basic_len = got;
				memcpy(last_basic, rxpkt, (size_t) got < sizeof(last_basic) ? (size_t) got : sizeof(last_basic));
			}
		} else {
			log_hex("CAPT: recv", rxpkt, (size_t) got, 48);
		}
		if (payload)
			*payload = rxpkt + 4;
		return got - 4;
	}
	ERR("CAPT: no matching reply to command %04X", cmd);
	return -1;
}

/* ---------------------------------------------------------------- status */

static void decode_status(const uint8_t *p, int len, bool extended)
{
	if (len >= 2)  st.s[0] = WORD(p + 0);
	if (len >= 6)  st.buflevel = p[4] & 0x0F;
	if (!extended)
		return;
	if (len >= 10) { st.s[1] = WORD(p + 8); st.aux = p[8]; st.cnt = p[9]; }
	if (len >= 12) { st.s[2] = WORD(p + 10); st.pap = p[10]; }
	if (len >= 14) st.s[3] = WORD(p + 12);
	if (len >= 22) {
		st.page_decoding  = WORD(p + 14);
		st.page_printing  = WORD(p + 16);
		st.page_out       = WORD(p + 18);
		st.page_completed = WORD(p + 20);
	}
	if (len >= 26) st.s[4] = WORD(p + 24);
	if (len >= 32) st.s[5] = WORD(p + 30);
	if (len >= 36) st.page_received = WORD(p + 34);
	if (len >= 40) st.s[6] = WORD(p + 38);
}

static int query(uint16_t cmd)
{
	const uint8_t *p;
	int len = capt_sendrecv(cmd, NULL, 0, &p);
	if (len < 0)
		return -1;
	decode_status(p, len, cmd == CAPT_CHKXSTATUS);
	return 0;
}

static int query_input(void)
{
	const uint8_t *p;
	int len = capt_sendrecv(CAPT_CHKJOBSTAT, NULL, 0, &p);
	if (len < 0)
		return -1;
	if (len >= 3) st.input_flags = p[2];
	if (len >= 6) st.input_paper = p[5];
	DBG("CAPT: input status: flags 0x%02X, paper code 0x%02X", st.input_flags, st.input_paper);
	return 0;
}

static void log_status(void)
{
	DBG("CAPT: status %04X aux %02X cnt %02X pap %02X buf %u | pages dec=%u prn=%u out=%u done=%u rcv=%u",
	    st.s[0], st.aux, st.cnt, st.pap, st.buflevel, st.page_decoding, st.page_printing,
	    st.page_out, st.page_completed, st.page_received);
}

/* basic status; extended/input status when the printer asks for it */
static int get_status(bool force_ext)
{
	if (query(CAPT_CHKSTATUS) < 0)
		return -1;
	for (int i = 0; i < 4; i++) {
		bool ext = force_ext || ST_XSTATUS_CHG(&st);
		bool inp = ST_NEED_INPUT(&st);
		force_ext = false;
		if (!ext && !inp)
			break;
		if (ext) {
			if (query(CAPT_CHKXSTATUS) < 0)
				return -1;
			log_status();
		}
		if (ST_NEED_INPUT(&st) && query_input() < 0)
			return -1;
		if (!ext)
			break;
	}
	return 0;
}

static int wait_ready(void)
{
	for (int i = 0; i < 600; i++) {
		if (get_status(false) < 0)
			return -1;
		if (!ST_BUSY(&st))
			return 0;
		sleep(1);
	}
	ERR("CAPT: printer stays busy");
	return -1;
}

static int release_printer(void);

static void msleep(unsigned ms)
{
	struct timespec ts = { ms / 1000, (long) (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------- commands */

static const uint8_t job_begin_magic[] = { 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const uint8_t upload2_magic[] = {
	0xEE, 0xDB, 0xEA, 0xAD, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* the job prologue that is known to work on the LBP2900 uses this GPIO block */
static const uint8_t gpio_job_init[] = {
	0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t gpio_led_off[] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t gpio_led_blink[] = {
	0x00, 0x00, 0x01, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00,
};

/* E1A1: job state notification. fg: 1 = job start, 2 = page sent, 6 = page fired, 4 = job end */
static int job_setup(uint8_t fg, uint16_t page)
{
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	uint8_t buf[72] = {
		0x00, 0x00, 0x00, 0x00, LO(page), HI(page), 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* host/user/doc name lengths = 0 */
		fg, 0x01, LO(job_id), HI(job_id),
		0xC4, 0xFF,  /* -60 */
		0x88, 0xFF,  /* -120 */
		LO((uint16_t) tm.tm_year), HI((uint16_t) tm.tm_year), (uint8_t) tm.tm_mon, (uint8_t) tm.tm_mday,
		(uint8_t) tm.tm_hour, (uint8_t) tm.tm_min, (uint8_t) tm.tm_sec, 0x01,
	};
	return capt_sendrecv(CAPT_JOB_SETUP, buf, sizeof(buf), NULL) < 0 ? -1 : 0;
}

/* ClearMisPrint, ClearError, DiscardData, GoOnline: brings the printer online,
 * resets its page counters */
static int go_online(void)
{
	DBG("CAPT: bringing printer online");
	if (capt_sendrecv(CAPT_START_1, NULL, 0, NULL) < 0 ||
	    capt_sendrecv(CAPT_START_2, NULL, 0, NULL) < 0 ||
	    capt_sendrecv(CAPT_START_3, NULL, 0, NULL) < 0 ||
	    wait_ready() < 0 ||
	    capt_sendrecv(CAPT_UPLOAD_2, upload2_magic, sizeof(upload2_magic), NULL) < 0 ||
	    wait_ready() < 0)
		return -1;
	last_out = 0;
	return 0;
}

/* GoOffline (only when online). Payload: job id, as Canon's Linux driver sends. */
static int go_offline(void)
{
	if (get_status(false) < 0)
		return -1;
	if (ST_B1(&st) & 0x10)
		return 0;  /* already offline */
	uint8_t jb[2] = { LO(job_id), HI(job_id) };
	DBG("CAPT: taking printer offline");
	if (capt_sendrecv(CAPT_GO_OFFLINE, jb, 2, NULL) < 0)
		return -1;
	for (int i = 0; i < 50; i++) {
		if (get_status(false) < 0)
			return -1;
		if (ST_B1(&st) & 0x10)
			return 0;
		msleep(100);
	}
	WARN("CAPT: printer did not go offline");
	return 0;
}

/* human-readable engine error from the extended status, or NULL */
static const char *engine_error(void)
{
	if (st.cnt & 0x01) return "data overrun";
	if (st.cnt & 0x02) return "data underrun";
	if (st.cnt & 0x04) return "missing end of page";
	if (st.cnt & 0x08) return "invalid data";
	if (st.cnt & 0x10) return "engine communication error";
	if ((st.s[3] & 0x4000)) return "cover open";
	if ((st.s[3] & 0x2000)) return "no toner cartridge";
	return NULL;
}

int capt_job_begin(void)
{
	const uint8_t *p;
	int len;

	memset(&st, 0, sizeof(st));
	st.buflevel = 15;
	st.pap = 0x80;
	last_out = 0;
	last_basic_len = -1;
	tr_drain_input();

	if (capt_sendrecv(CAPT_IDENT, NULL, 0, NULL) < 0)
		return -1;
	sleep(1);
	if (get_status(false) < 0)
		return -1;
	if (capt_sendrecv(CAPT_START_0, NULL, 0, NULL) < 0)
		return -1;
	len = capt_sendrecv(CAPT_JOB_BEGIN, job_begin_magic, sizeof(job_begin_magic), &p);
	if (len < 0)
		return -1;
	job_id = len >= 4 ? WORD(p + 2) : 0;
	DBG("CAPT: job id %u", job_id);

	if (capt_sendrecv(CAPT_GPIO, gpio_job_init, sizeof(gpio_job_init), NULL) < 0)
		return -1;
	if (wait_ready() < 0)
		return -1;
	if (job_setup(1, 0) < 0)
		return -1;
	if (wait_ready() < 0)
		return -1;
	/* always start from a clean state, like Canon's drivers: offline, clear
	 * errors, discard leftover data, online (resets the page counters) */
	if (go_offline() < 0 || go_online() < 0)
		return -1;
	return get_status(true);
}

int capt_page_begin(const struct page_params *pp)
{
	uint8_t fm; /* fuser mode */
	switch (pp->media_type) {
	case 3: fm = 0x02; break;  /* heavy paper H */
	case 4: fm = 0x13; break;  /* transparency */
	default: fm = 0x01; break; /* plain, heavy, plain L */
	}
	const uint8_t air = 0x02;  /* automatic image refinement */
	const uint8_t ink_k = 0x00;

	uint8_t pageparms[40] = {
		0x00, 0x00, 0x30, 0x2A, pp->size_code, 0x00, 0x00, 0x00,
		ink_k, 0x1C, 0x1C, 0x1C, pp->media_type, pp->media_adapt, 0x04, 0x00,
		0x01, 0x01, air, pp->toner_save, 0x00, 0x00,
		LO((uint16_t) pp->margin_h), HI((uint16_t) pp->margin_h),
		LO((uint16_t) pp->margin_w), HI((uint16_t) pp->margin_w),
		LO((uint16_t) pp->line_size), HI((uint16_t) pp->line_size),
		LO((uint16_t) pp->num_lines), HI((uint16_t) pp->num_lines),
		LO((uint16_t) pp->paper_width), HI((uint16_t) pp->paper_width),
		LO((uint16_t) pp->paper_height), HI((uint16_t) pp->paper_height),
		0x00, 0x00, fm, 0x00, 0x00, 0x00,
	};

	page_fired = false;
	if (get_status(true) < 0)
		return -1;
	if (ST_UNINIT(&st) && go_online() < 0)
		return -1;

	for (int i = 0;; i++) {
		if (get_status(false) < 0)
			return -1;
		if (!ST_BUFFERFULL(&st) && st.buflevel > 0)
			break;
		if (i > 600) {
			ERR("CAPT: printer buffer stays full");
			return -1;
		}
		sleep(1);
	}

	/* D0A9 container with page parameters, compression parameters and two empty blocks */
	size_t n = 4;
	uint8_t hp[16];
	size_t hl = hiscoa_format_params(hp, sizeof(hp));
	n += frame(txbuf + n, CAPT_SET_PARM_PAGE, pageparms, sizeof(pageparms));
	n += frame(txbuf + n, CAPT_SET_PARM_HISCOA, hp, hl);
	n += frame(txbuf + n, CAPT_SET_PARM_1, NULL, 0);
	n += frame(txbuf + n, CAPT_SET_PARM_2, NULL, 0);
	txbuf[0] = LO(CAPT_SET_PARMS);
	txbuf[1] = HI(CAPT_SET_PARMS);
	txbuf[2] = LO((uint16_t) n);
	txbuf[3] = HI((uint16_t) n);
	log_hex("CAPT: send", txbuf, n, 80);
	return tr_write(txbuf, n);
}

/* start the engine on the page currently being received */
static int fire_page(void)
{
	if (get_status(true) < 0)
		return -1;
	uint16_t page = st.page_decoding;
	if (job_setup(2, page) < 0 || wait_ready() < 0)
		return -1;
	uint8_t pb[2] = { LO(page), HI(page) };
	if (capt_sendrecv(CAPT_FIRE, pb, 2, NULL) < 0)
		return -1;
	page_fired = true;
	fired_page = page;
	return 0;
}

/* before sending a chunk: honour flow control; returns 0, 1 (paper out), -1 */
static int flow_control(void)
{
	if (get_status(false) < 0)
		return -1;
	time_t start = time(NULL);
	bool streaming_logged = false;
	while (st.buflevel == 0) {
		if (g_cancelled)
			return -1;
		if (!page_fired) {
			DBG("CAPT: printer buffer full - streaming mode, starting engine early");
			if (fire_page() < 0)
				return -1;
		} else if (!streaming_logged) {
			DBG("CAPT: waiting for printer buffer to drain");
			streaming_logged = true;
		}
		if (time(NULL) - start > 180) {
			ERR("CAPT: printer buffer does not drain");
			return -1;
		}
		msleep(50);
		if (get_status(false) < 0)
			return -1;
		if (ST_NOPAPER(&st))
			break;
	}
	if (ST_NOPAPER(&st)) {
		WARN("CAPT: out of paper");
		return 1;
	}
	return 0;
}

int capt_send_band(const uint8_t *data, size_t size, unsigned *chunks)
{
	while (size) {
		if (g_cancelled)
			return -1;
		size_t c = size > DATA_CHUNK ? DATA_CHUNK : size;
		++*chunks;
		if (g_opts.legacy_poll) {
			if (*chunks % 16 == 0 && wait_ready() < 0)
				return -1;
		} else {
			int f = flow_control();
			if (f)
				return f;
		}
		if (capt_send(CAPT_PRINT_DATA, data, c) < 0) {
			get_status(true);
			ERR("CAPT: printer stopped accepting page data after %u chunks (status %04X, buffer %u, input 0x%02X/0x%02X)",
			    *chunks, st.s[0], st.buflevel, st.input_flags, st.input_paper);
			return -1;
		}
		data += c;
		size -= c;
	}
	return 0;
}

int capt_page_end(void)
{
	if (capt_send(CAPT_PRINT_DATA_END, NULL, 0) < 0)
		return -1;

	uint16_t page;
	if (page_fired) {
		page = fired_page;
	} else {
		/* wait until the printer has received the whole page */
		for (int i = 0;; i++) {
			msleep(i < 10 ? 100 : 1000);
			if (get_status(i >= 5) < 0)
				return -1;
			if (st.page_received == st.page_decoding)
				break;
			if (i > 300) {
				ERR("CAPT: printer did not accept the page");
				return -1;
			}
		}
		if (fire_page() < 0)
			return -1;
		page = fired_page;
	}
	if (wait_ready() < 0)
		return -1;
	if (job_setup(6, page) < 0)
		return -1;

	/* wait until the sheet has left the printer. Normally page_out reaches the
	 * page number; after a lost page the counters may lag, so any advance of
	 * page_out also counts. */
	uint16_t out_before = st.page_out;
	for (int i = 0;; i++) {
		if (get_status(i >= 10 && i % 5 == 0) < 0)
			return -1;
		if (st.page_out == page || st.page_out != out_before) {
			last_out = st.page_out;
			return 0;
		}
		const char *err = engine_error();
		if (err) {
			ERR("CAPT: printer reports %s (cnt 0x%02X, engine 0x%04X)", err, st.cnt, st.s[3]);
			return -1;
		}
		if (ST_NOPAPER(&st) && !ST_PRINTING(&st)) {
			WARN("CAPT: out of paper");
			return 1;
		}
		if (i > 300) {
			ERR("CAPT: page did not come out (paper jam?)");
			return -1;
		}
		sleep(1);
	}
}

int capt_job_end(void)
{
	for (int i = 0;; i++) {
		if (get_status(i >= 5) < 0)
			return -1;
		if (st.page_completed == st.page_decoding ||
		    (last_out && st.page_completed == last_out))
			break;
		if (i > 300) {
			WARN("CAPT: printer did not report job completion");
			break;
		}
		sleep(1);
	}
	if (job_setup(4, st.page_completed) < 0)
		return -1;
	return release_printer() < 0 ? -1 : 0;
}

/* job end as in Canon's Linux driver: ClearError, DiscardData, GoOffline, ReleaseUnit */
static int release_printer(void)
{
	uint8_t jb[2] = { LO(job_id), HI(job_id) };
	int rc = 0;
	if (capt_sendrecv(CAPT_START_2, NULL, 0, NULL) < 0 ||
	    capt_sendrecv(CAPT_START_3, NULL, 0, NULL) < 0)
		rc = -1;
	if (go_offline() < 0)
		rc = -1;
	if (capt_sendrecv(CAPT_JOB_END, jb, 2, NULL) < 0)
		rc = -1;
	return rc;
}

void capt_job_abort(void)
{
	if (get_status(true) < 0)
		return;
	capt_sendrecv(CAPT_GPIO, gpio_led_off, sizeof(gpio_led_off), NULL);
	job_setup(4, st.page_completed);
	/* also clear a misprint so the next job starts cleanly */
	capt_sendrecv(CAPT_START_1, NULL, 0, NULL);
	release_printer();
}

/*
 * Out of paper: blink the LED, wait until the user loads paper and presses the
 * button, then clear the error and bring the printer back online (this resets
 * the page counters). The caller re-sends the lost page.
 */
int capt_recover_paper(void)
{
	if (capt_sendrecv(CAPT_GPIO, gpio_led_blink, sizeof(gpio_led_blink), NULL) < 0)
		return -1;
	if (get_status(true) < 0 || query_input() < 0)
		return -1;
	bool saw_error = !(st.input_flags & 0x40);
	for (;;) {
		if (g_cancelled)
			return -1;
		sleep(1);
		if (get_status(true) < 0)
			return -1;
		if (ST_BUTTON(&st))
			break;
		if (saw_error && (st.input_flags & 0x40))
			break;  /* engine ready again after the button press */
		if (!(st.input_flags & 0x40))
			saw_error = true;
	}
	DBG("CAPT: button pressed, resuming");
	if (capt_sendrecv(CAPT_GPIO, gpio_led_off, sizeof(gpio_led_off), NULL) < 0)
		return -1;
	return go_online();
}
