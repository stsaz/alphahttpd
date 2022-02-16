/** Light config parser
2022, Simon Zolin
*/

/*
Format:

	[SPACE] KEY [SPACE VAL]... [SPACE] LF

SPACE: tab, space or CR characters
KEY, VAL: word consisting of printable characters
*/

#pragma once

#include <ffbase/string.h>

struct ltconf {
	ffuint line, line_char; // line# and character# starting at 0
	unsigned have_key;
};

enum LTCONF_R {
	/** Need more input data */
	LTCONF_MORE,

	/** The first word on a new line */
	LTCONF_KEY,

	/** One or several words after the key */
	LTCONF_VAL,

	/** Reached invalid control character */
	LTCONF_ERROR,
};

/** Read and consume input data and set key/value output data.
Return enum LTCONF_R */
static inline int ltconf_read(struct ltconf *c, ffstr *in, ffstr *out)
{
	const char *d = in->ptr, *end = in->ptr + in->len;
	while (d != end) {
		if (*d == '\n') {
			c->line++;
			c->line_char = 0;
			c->have_key = 0;
		} else if (!(*d == ' ' || *d == '\t' || *d == '\r')) {
			break;
		}
		d++;
	}
	ffstr_shift(in, d - in->ptr);

	ffssize r = ffs_skip_ranges(d, end - d, "\x21\x7e\x80\xff", 4); // printable
	if (r < 0)
		return LTCONF_MORE;
	else if (r == 0)
		return LTCONF_ERROR;

	ffstr_set(out, d, r);
	ffstr_shift(in, r);
	c->line_char += r;

	if (!c->have_key) {
		c->have_key = 1;
		return LTCONF_KEY;
	}
	return LTCONF_VAL;
}
