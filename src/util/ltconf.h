/** Light config parser
2022, Simon Zolin
*/

/*
Format:

	[SPACE] KEY [SPACE VAL]... [SPACE #COMMENT] LF

SPACE: tab, space or CR characters
KEY, VAL: word consisting of printable characters
*/

#pragma once

#include <ffbase/string.h>

struct ltconf {
	ffuint line, line_char; // line# and character# starting at 0
	unsigned have_key :1;
	unsigned comment :1;
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

	for (;;) {
		if (c->comment) {
			if (NULL == (d = ffmem_findbyte(d, end - d, '\n')))
				d = end;
			else
				c->comment = 0;
		}

		if (d == end) {
			ffstr_shift(in, in->len);
			return LTCONF_MORE;
		}

		// usually there are just a few bytes of whitespace - we don't need ffs_skip_ranges() here
		switch (*d) {
		case '\n':
			c->line++;
			c->line_char = 0;
			c->have_key = 0;
			break;

		case ' ': case '\t': case '\r':
			break;

		case '#':
			c->comment = 1;
			break;

		default:
			goto after_space;
		}
		d++;
	}

after_space:
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
