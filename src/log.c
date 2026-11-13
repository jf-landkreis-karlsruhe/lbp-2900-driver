/* Logging: CUPS reads "LEVEL: message" lines from a backend's stderr. */
#include "capt.h"

#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

static FILE *logfile;

void log_open_file(const char *path)
{
	if (!path || !*path)
		return;
	logfile = fopen(path, "w");
	if (logfile) {
		chmod(path, 0644);
		setvbuf(logfile, NULL, _IOLBF, 0);
	}
}

static void vlog(FILE *f, const char *level, const char *fmt, va_list ap)
{
	fprintf(f, "%s: ", level);
	vfprintf(f, fmt, ap);
	fputc('\n', f);
}

void log_msg(const char *level, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vlog(stderr, level, fmt, ap);
	va_end(ap);
	if (logfile) {
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		struct tm tm;
		localtime_r(&ts.tv_sec, &tm);
		fprintf(logfile, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec,
		        ts.tv_nsec / 1000000);
		va_start(ap, fmt);
		vlog(logfile, level, fmt, ap);
		va_end(ap);
	}
}

void log_hex(const char *prefix, const uint8_t *buf, size_t len, size_t max)
{
	char line[3 * 48 + 64];
	size_t n = len < max ? len : max;
	size_t pos = 0;
	for (size_t i = 0; i < n && pos + 4 < sizeof(line); i++)
		pos += snprintf(line + pos, sizeof(line) - pos, " %02X", buf[i]);
	line[pos] = '\0';
	if (n < len)
		log_msg("DEBUG", "%s%s ... (%zu bytes)", prefix, line, len);
	else
		log_msg("DEBUG", "%s%s", prefix, line);
}

/* "STATE: +media-empty-error" etc. */
void cups_state(const char *fmt, ...)
{
	va_list ap;
	fputs("STATE: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}
