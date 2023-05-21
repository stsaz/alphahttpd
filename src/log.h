/** alphahttpd: logging
2022, Simon Zolin */

#include <FFOS/std.h>
#include <FFOS/thread.h>

extern fftime sv_date(alphahttpd *s, ffstr *dts);

static const char* color_get(uint level)
{
	static const char colors[][8] = {
		/*ALPHAHTTPD_LOG_SYSFATAL*/	FFSTD_CLR_B(FFSTD_RED),
		/*ALPHAHTTPD_LOG_SYSERR*/	FFSTD_CLR(FFSTD_RED),
		/*ALPHAHTTPD_LOG_ERR*/	FFSTD_CLR(FFSTD_RED),
		/*ALPHAHTTPD_LOG_SYSWARN*/	FFSTD_CLR(FFSTD_YELLOW),
		/*ALPHAHTTPD_LOG_WARN*/	FFSTD_CLR(FFSTD_YELLOW),
		/*ALPHAHTTPD_LOG_INFO*/	FFSTD_CLR(FFSTD_GREEN),
		/*ALPHAHTTPD_LOG_VERBOSE*/	FFSTD_CLR(FFSTD_GREEN),
		/*ALPHAHTTPD_LOG_DEBUG*/	"",
		/*ALPHAHTTPD_LOG_EXTRA*/	FFSTD_CLR_I(FFSTD_BLUE),
	};
	return colors[level];
}

// TIME LEVEL #TID [ID:] MSG [: SYSERR]
void ahd_logv(void *opaque, uint level, const char *id, const char *fmt, va_list va)
{
	char buf[1024];
	ffsize r = 0, cap = sizeof(buf) - 2;

	const char *color_end = "";
	if (boss->stdout_color) {
		const char *color = color_get(level);
		if (color[0] != '\0') {
			r = _ffs_copyz(buf, cap, color);
			color_end = FFSTD_CLR_RESET;
		}
	}

	// time
	ffstr dts = {};
	if (opaque != NULL)
		sv_date((alphahttpd*)opaque, &dts);
	r += _ffs_copy(&buf[r], cap - r, dts.ptr, dts.len);

	// level
	buf[r++] = ' ';
	static const char level_str[][8] = {
		"FATAL",
		"ERROR",
		"ERROR",
		"WARNING",
		"WARNING",
		"INFO",
		"VERBOSE",
		"DEBUG",
		"DEBUG",
	};
	FF_ASSERT(FF_COUNT(level_str) == ALPHAHTTPD_LOG_EXTRA+1);
	r += _ffs_copyz(&buf[r], cap - r, level_str[level]);
	buf[r++] = '\t';

	ffuint64 tid = ffthread_curid();
	buf[r++] = '#';
	r += ffs_fromint(tid, &buf[r], cap - r, 0);
	buf[r++] = '\t';

	if (id != NULL) {
		r += _ffs_copyz(&buf[r], cap - r, id);
		buf[r++] = ':';
		buf[r++] = ' ';
	}

	ffssize r2 = ffs_formatv(&buf[r], cap - r, fmt, va);
	if (r2 < 0)
		r2 = 0;
	r += r2;

	if (level == ALPHAHTTPD_LOG_SYSFATAL
		|| level == ALPHAHTTPD_LOG_SYSERR
		|| level == ALPHAHTTPD_LOG_SYSWARN) {
		r += ffs_format_r0(&buf[r], cap - r, ": (%u) %s"
			, fferr_last(), fferr_strptr(fferr_last()));
	}

	r += _ffs_copyz(&buf[r], cap - r, color_end);

	buf[r++] = '\n';
	ffstdout_write(buf, r);
}

/** Add line to log
level: enum LOG */
void ahd_log(void *opaque, uint level, const char *id, const char *fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	ahd_logv(opaque, level, id, fmt, va);
	va_end(va);
}
