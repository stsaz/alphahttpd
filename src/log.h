/** alphahttpd: logging
2022, Simon Zolin */

#include <FFOS/std.h>

// TIME LEVEL: [ID:] MSG [: SYSERR]
void ahd_log(uint level, const char *id, const char *fmt, ...)
{
	char buf[1024];
	ffsize cap = sizeof(buf) - 2;

	// time
	ffstr dts;
	sv_date(NULL, &dts);
	ffsize r = _ffs_copy(buf, cap, dts.ptr, dts.len);

	// level
	buf[r++] = ' ';
	static const char level_str[][7] = {
		"FATAL",
		"ERR",
		"ERR",
		"WARN",
		"WARN",
		"INFO",
		"VERB",
		"DBG",
	};
	FF_ASSERT(FF_COUNT(level_str) == LOG_DBG+1);
	r += _ffs_copyz(&buf[r], cap - r, level_str[level]);
	buf[r++] = ':';
	buf[r++] = '\t';

	if (id != NULL) {
		r += _ffs_copyz(&buf[r], cap - r, id);
		buf[r++] = ':';
		buf[r++] = ' ';
	}

	va_list args;
	va_start(args, fmt);
	ffssize r2 = ffs_formatv(&buf[r], cap - r, fmt, args);
	va_end(args);
	if (r2 < 0)
		r2 = 0;
	r += r2;

	if (level == LOG_SYSFATAL
		|| level == LOG_SYSERR
		|| level == LOG_SYSWARN) {
		r += ffs_format_r0(&buf[r], cap - r, ": (%u) %s"
			, fferr_last(), fferr_strptr(fferr_last()));
	}

	buf[r++] = '\n';
	if (level <= LOG_WARN)
		ffstderr_write(buf, r);
	else
		ffstdout_write(buf, r);
}
