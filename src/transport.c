/*
 * Device discovery (sysfs) and USB I/O.
 *
 * Two transports are supported:
 *   - usblp: the kernel printer driver, /dev/usb/lpN (bidirectional read/write)
 *   - usbfs: raw bulk transfers via /dev/bus/usb/BBB/DDD, used when usblp is
 *            not bound (e.g. blacklisted on some distributions)
 * plus a Unix-socket transport ("capt-sim:/path") used by the test simulator.
 */
#include "capt.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usbdevice_fs.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SYSFS_USB "/sys/bus/usb/devices"
#define CANON_VID 0x04a9

enum tr_kind { TR_NONE, TR_USBLP, TR_USBFS, TR_SOCKET };

static enum tr_kind tr_kind = TR_NONE;
static int tr_fd = -1;
static int tr_ifnum, tr_ep_in, tr_ep_out;
static uint8_t rxbuf[0x10000];
int tr_write_timeout_ms = 60000;
static size_t rx_pos, rx_len;

/* ------------------------------------------------------------------ sysfs */

static int read_sysfs(const char *dir, const char *name, char *out, size_t size)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;
	size_t n = fread(out, 1, size - 1, f);
	fclose(f);
	out[n] = '\0';
	while (n && isspace((unsigned char) out[n - 1]))
		out[--n] = '\0';
	return 0;
}

static long read_sysfs_num(const char *dir, const char *name, int base)
{
	char buf[32];
	if (read_sysfs(dir, name, buf, sizeof(buf)) < 0)
		return -1;
	return strtol(buf, NULL, base);
}

/* Extract value of key (e.g. "MDL") from an IEEE 1284 device ID string */
static void ieee_field(const char *id, const char *key, const char *alt, char *out, size_t size)
{
	out[0] = '\0';
	const char *p = id;
	while (p && *p) {
		const char *end = strchr(p, ';');
		const char *colon = strchr(p, ':');
		if (!end)
			end = p + strlen(p);
		if (colon && colon < end) {
			size_t klen = colon - p;
			while (klen && isspace((unsigned char) p[klen - 1]))
				klen--;
			if ((strlen(key) == klen && !strncmp(p, key, klen)) ||
			    (alt && strlen(alt) == klen && !strncmp(p, alt, klen))) {
				size_t vlen = end - colon - 1;
				if (vlen >= size)
					vlen = size - 1;
				memcpy(out, colon + 1, vlen);
				out[vlen] = '\0';
				return;
			}
		}
		p = *end ? end + 1 : NULL;
	}
}

static int usbfs_get_device_id(struct capt_device *d);

static bool probe_device(const char *sysname, struct capt_device *d)
{
	char dir[256], buf[256];
	memset(d, 0, sizeof(*d));
	snprintf(dir, sizeof(dir), SYSFS_USB "/%s", sysname);
	if (read_sysfs_num(dir, "idVendor", 16) != CANON_VID)
		return false;

	/* find a printer-class interface (class 7) */
	DIR *dd = opendir(dir);
	if (!dd)
		return false;
	struct dirent *de;
	char ifdir[512] = "";
	while ((de = readdir(dd))) {
		if (!strchr(de->d_name, ':'))
			continue;
		char p[512];
		snprintf(p, sizeof(p), "%s/%s", dir, de->d_name);
		if (read_sysfs_num(p, "bInterfaceClass", 16) == 7) {
			snprintf(ifdir, sizeof(ifdir), "%s", p);
			break;
		}
	}
	closedir(dd);
	if (!ifdir[0])
		return false;

	snprintf(d->sysname, sizeof(d->sysname), "%s", sysname);
	d->busnum = (int) read_sysfs_num(dir, "busnum", 10);
	d->devnum = (int) read_sysfs_num(dir, "devnum", 10);
	d->ifnum = (int) read_sysfs_num(ifdir, "bInterfaceNumber", 16);
	read_sysfs(dir, "serial", d->serial, sizeof(d->serial));

	/* endpoints */
	d->ep_in = d->ep_out = -1;
	dd = opendir(ifdir);
	if (dd) {
		while ((de = readdir(dd))) {
			if (strncmp(de->d_name, "ep_", 3))
				continue;
			char p[600];
			snprintf(p, sizeof(p), "%s/%s", ifdir, de->d_name);
			if (read_sysfs(p, "type", buf, sizeof(buf)) < 0 || strcmp(buf, "Bulk"))
				continue;
			int addr = (int) strtol(de->d_name + 3, NULL, 16);
			if (addr & 0x80)
				d->ep_in = addr;
			else
				d->ep_out = addr;
		}
		closedir(dd);
	}

	/* usblp node, if the kernel driver is bound */
	char miscdir[600];
	snprintf(miscdir, sizeof(miscdir), "%s/usbmisc", ifdir);
	dd = opendir(miscdir);
	if (dd) {
		while ((de = readdir(dd))) {
			if (!strncmp(de->d_name, "lp", 2)) {
				snprintf(d->lp_node, sizeof(d->lp_node), "/dev/usb/%.40s", de->d_name);
				break;
			}
		}
		closedir(dd);
	}

	/* IEEE 1284 device ID: from usblp sysfs, else via a control request */
	if (read_sysfs(ifdir, "ieee1284_id", d->ieee1284, sizeof(d->ieee1284)) < 0 || !d->ieee1284[0])
		usbfs_get_device_id(d);

	if (d->ieee1284[0]) {
		char cmd[64];
		ieee_field(d->ieee1284, "CMD", "COMMAND SET", cmd, sizeof(cmd));
		if (cmd[0] && !strstr(cmd, "CAPT"))
			return false; /* some other Canon printer */
		ieee_field(d->ieee1284, "MDL", "MODEL", d->model, sizeof(d->model));
	} else {
		read_sysfs(dir, "product", buf, sizeof(buf));
		if (!strstr(buf, "CAPT"))
			return false;
	}
	if (!d->model[0])
		snprintf(d->model, sizeof(d->model), "CAPT-%04lx", read_sysfs_num(dir, "idProduct", 16));
	return true;
}

int dev_enumerate(struct capt_device *out, int max)
{
	DIR *dd = opendir(SYSFS_USB);
	if (!dd)
		return 0;
	int n = 0;
	struct dirent *de;
	while ((de = readdir(dd)) && n < max) {
		if (strchr(de->d_name, ':') || !isdigit((unsigned char) de->d_name[0]))
			continue;
		if (probe_device(de->d_name, &out[n]))
			n++;
	}
	closedir(dd);
	return n;
}

static void url_decode(const char *in, char *out, size_t size)
{
	size_t o = 0;
	for (; *in && o + 1 < size; in++) {
		if (*in == '%' && isxdigit((unsigned char) in[1]) && isxdigit((unsigned char) in[2])) {
			char h[3] = { in[1], in[2], 0 };
			out[o++] = (char) strtol(h, NULL, 16);
			in += 2;
		} else {
			out[o++] = *in;
		}
	}
	out[o] = '\0';
}

/*
 * URI forms:
 *   capt://Canon/LBP2900?serial=0000A355QEVM   (preferred, stable)
 *   capt://Canon/LBP2900                       (first device with that model)
 *   capt:/dev/usb/lp0                          (explicit usblp node)
 */
int dev_find(const char *uri, struct capt_device *dev)
{
	struct capt_device devs[16];
	int n = dev_enumerate(devs, ARRAY_SIZE(devs));
	char serial[64] = "", model[64] = "";

	if (!strncmp(uri, "capt:/dev/", 10)) {
		for (int i = 0; i < n; i++) {
			if (!strcmp(devs[i].lp_node, uri + 5)) {
				*dev = devs[i];
				return 0;
			}
		}
		memset(dev, 0, sizeof(*dev));
		snprintf(dev->lp_node, sizeof(dev->lp_node), "%s", uri + 5);
		return access(dev->lp_node, F_OK) == 0 ? 0 : -1;
	}

	if (!strncmp(uri, "capt://", 7)) {
		const char *p = strchr(uri + 7, '/');
		if (p) {
			const char *q = strchr(p, '?');
			char tmp[64];
			size_t len = q ? (size_t) (q - p - 1) : strlen(p + 1);
			if (len >= sizeof(tmp))
				len = sizeof(tmp) - 1;
			memcpy(tmp, p + 1, len);
			tmp[len] = '\0';
			url_decode(tmp, model, sizeof(model));
			if (q && !strncmp(q, "?serial=", 8))
				url_decode(q + 8, serial, sizeof(serial));
		}
	}

	for (int i = 0; i < n; i++) {
		if (serial[0] && strcmp(devs[i].serial, serial))
			continue;
		if (!serial[0] && model[0] && strcmp(devs[i].model, model))
			continue;
		*dev = devs[i];
		return 0;
	}
	return -1;
}

/* ------------------------------------------------------------------ usbfs */

static int usbfs_path(const struct capt_device *d, char *buf, size_t size)
{
	if (d->busnum <= 0 || d->devnum <= 0)
		return -1;
	snprintf(buf, size, "/dev/bus/usb/%03d/%03d", d->busnum, d->devnum);
	return 0;
}

static int usbfs_get_device_id(struct capt_device *d)
{
	char path[64];
	if (usbfs_path(d, path, sizeof(path)) < 0)
		return -1;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	uint8_t buf[1024];
	struct usbdevfs_ctrltransfer ct = {
		.bRequestType = 0xA1, /* class, interface, device-to-host */
		.bRequest = 0,        /* GET_DEVICE_ID */
		.wValue = 0,
		.wIndex = (uint16_t) (d->ifnum << 8),
		.wLength = sizeof(buf),
		.timeout = 2000,
		.data = buf,
	};
	int r = ioctl(fd, USBDEVFS_CONTROL, &ct);
	close(fd);
	if (r < 2)
		return -1;
	size_t len = (size_t) ((buf[0] << 8) | buf[1]);
	if (len > (size_t) r)
		len = (size_t) r;
	if (len < 2)
		return -1;
	len -= 2;
	if (len >= sizeof(d->ieee1284))
		len = sizeof(d->ieee1284) - 1;
	memcpy(d->ieee1284, buf + 2, len);
	d->ieee1284[len] = '\0';
	return 0;
}

static int usbfs_open(const struct capt_device *d)
{
	char path[64];
	if (usbfs_path(d, path, sizeof(path)) < 0 || d->ep_in < 0 || d->ep_out < 0)
		return -1;
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		ERR("cannot open %s: %s", path, strerror(errno));
		return -1;
	}
	struct usbdevfs_ioctl cmd = { .ifno = d->ifnum, .ioctl_code = USBDEVFS_DISCONNECT, .data = NULL };
	ioctl(fd, USBDEVFS_IOCTL, &cmd); /* detach usblp if bound; ignore errors */
	unsigned ifnum = (unsigned) d->ifnum;
	if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifnum) < 0) {
		ERR("cannot claim USB interface %d: %s", d->ifnum, strerror(errno));
		close(fd);
		return -1;
	}
	tr_fd = fd;
	tr_ifnum = d->ifnum;
	tr_ep_in = d->ep_in;
	tr_ep_out = d->ep_out;
	tr_kind = TR_USBFS;
	DBG("CAPT: using usbfs %s (if %d, in 0x%02x, out 0x%02x)", path, d->ifnum, d->ep_in, d->ep_out);
	return 0;
}

/* ------------------------------------------------------------- open/close */

int tr_open(const struct capt_device *d)
{
	rx_pos = rx_len = 0;
	const char *force = getenv("CAPT_TRANSPORT");
	bool want_usbfs = force && !strcmp(force, "usbfs");

	if (!want_usbfs && d->lp_node[0]) {
		int fd = open(d->lp_node, O_RDWR | O_CLOEXEC | O_NONBLOCK);
		if (fd >= 0) {
			tr_fd = fd;
			tr_kind = TR_USBLP;
			DBG("CAPT: using usblp %s", d->lp_node);
			return 0;
		}
		WARN("cannot open %s: %s, trying usbfs", d->lp_node, strerror(errno));
	}
	return usbfs_open(d);
}

int tr_open_uri_special(const char *uri)
{
	if (strncmp(uri, "capt-sim:", 9))
		return -1;
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", uri + 9);
	if (connect(fd, (struct sockaddr *) &sa, sizeof(sa)) < 0) {
		ERR("cannot connect to simulator %s: %s", sa.sun_path, strerror(errno));
		close(fd);
		return -1;
	}
	tr_fd = fd;
	tr_kind = TR_SOCKET;
	rx_pos = rx_len = 0;
	return 0;
}

void tr_close(void)
{
	if (tr_fd < 0)
		return;
	if (tr_kind == TR_USBFS) {
		unsigned ifnum = (unsigned) tr_ifnum;
		ioctl(tr_fd, USBDEVFS_RELEASEINTERFACE, &ifnum);
		struct usbdevfs_ioctl cmd = { .ifno = tr_ifnum, .ioctl_code = USBDEVFS_CONNECT, .data = NULL };
		ioctl(tr_fd, USBDEVFS_IOCTL, &cmd); /* give the device back to usblp */
	}
	close(tr_fd);
	tr_fd = -1;
	tr_kind = TR_NONE;
}

/* ------------------------------------------------------------------- I/O */

int tr_write(const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len) {
		size_t chunk = len > 16384 ? 16384 : len;
		ssize_t r;
		if (tr_kind == TR_USBFS) {
			struct usbdevfs_bulktransfer bt = {
				.ep = (unsigned) tr_ep_out, .len = (unsigned) chunk,
				.timeout = (unsigned) tr_write_timeout_ms, .data = (void *) p,
			};
			r = ioctl(tr_fd, USBDEVFS_BULK, &bt);
			if (r < 0 && errno == ETIMEDOUT) {
				ERR("CAPT: printer does not accept data (timeout)");
				return -1;
			}
		} else {
			/* never block forever: wait until the device can take data */
			struct pollfd pfd = { .fd = tr_fd, .events = POLLOUT };
			int pr = poll(&pfd, 1, tr_write_timeout_ms);
			if (pr < 0 && errno == EINTR) {
				if (g_cancelled)
					return -1;
				continue;
			}
			if (pr == 0) {
				ERR("CAPT: printer does not accept data (timeout after %d s)", tr_write_timeout_ms / 1000);
				return -1;
			}
			if (pfd.revents & (POLLERR | POLLHUP)) {
				ERR("CAPT: printer connection error");
				return -1;
			}
			r = write(tr_fd, p, chunk);
		}
		if (r < 0) {
			if (errno == EINTR && g_cancelled)
				return -1;
			if (errno == EINTR || errno == EAGAIN)
				continue;
			ERR("CAPT: write to printer failed: %s", strerror(errno));
			return -1;
		}
		if (r == 0) {
			ERR("CAPT: printer accepted no data");
			return -1;
		}
		p += r;
		len -= (size_t) r;
	}
	return 0;
}

/* fill rxbuf with whatever the device delivers within timeout_ms; returns bytes or -1 / 0 */
static ssize_t raw_read(int timeout_ms)
{
	if (tr_kind == TR_USBFS) {
		for (;;) {
			struct usbdevfs_bulktransfer bt = {
				.ep = (unsigned) tr_ep_in, .len = 16384,
				.timeout = (unsigned) (timeout_ms > 0 ? timeout_ms : 1), .data = rxbuf,
			};
			int r = ioctl(tr_fd, USBDEVFS_BULK, &bt);
			if (r < 0 && errno == EINTR)
				continue;
			if (r < 0 && errno == ETIMEDOUT)
				return 0;
			return r;
		}
	}
	struct pollfd pfd = { .fd = tr_fd, .events = POLLIN };
	for (;;) {
		int r = poll(&pfd, 1, timeout_ms);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return r;
		ssize_t n = read(tr_fd, rxbuf, sizeof(rxbuf));
		if (n < 0 && (errno == EINTR || errno == EAGAIN))
			continue;
		if (n == 0 && tr_kind == TR_SOCKET) {
			errno = EPIPE;
			return -1;
		}
		return n;
	}
}

ssize_t tr_read(void *buf, size_t len, int timeout_ms)
{
	if (rx_pos >= rx_len) {
		ssize_t n = raw_read(timeout_ms);
		if (n <= 0)
			return n;
		rx_pos = 0;
		rx_len = (size_t) n;
	}
	size_t avail = rx_len - rx_pos;
	if (len > avail)
		len = avail;
	memcpy(buf, rxbuf + rx_pos, len);
	rx_pos += len;
	return (ssize_t) len;
}

void tr_drain_input(void)
{
	uint8_t tmp[512];
	ssize_t n;
	while ((n = tr_read(tmp, sizeof(tmp), 50)) > 0)
		log_hex("CAPT: discarding stale input:", tmp, (size_t) n, 32);
}
