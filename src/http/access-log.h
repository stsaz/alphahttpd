/** alphahttpd: write access log
2022, Simon Zolin */

#include <FFOS/std.h>

/* CLIENT_IP REQ_TOTAL "METHOD PATH VER" RESP_TOTAL "VER CODE MSG" REALTIME */
static int accesslog_open(struct client *c)
{
	ffstr dts;
	fftime end_time = sv_date(c->srv, &dts);
	uint tms = end_time.sec*1000 + end_time.nsec/1000000 - c->start_time_msec;

	ffstr req_line;
	req_line = range16_tostr(&c->req.line, c->req.buf.ptr);

	ffvec v = {};
	if (NULL == ffvec_alloc(&v, 500 + req_line.len, 1)) {
		cl_warnlog(c, "no memory");
		return CHAIN_DONE;
	}
	char *d = v.ptr, *end = d + v.cap - 1;
	d += ffip46_tostr((void*)c->peer_ip, d, end - d);
	*d++ = '\t';
	d += ffs_format_r0(d, end - d, "%S \"%S\" %u %U %U %ums\n"
		, &dts
		, &req_line, (int)c->resp.code
		, c->req.transferred, c->resp.transferred
		, tms);
	ffstderr_write(c->file.buf.ptr, d - (char*)c->file.buf.ptr);
	ffvec_free(&v);
	return CHAIN_DONE;
}

static const struct ahd_mod accesslog_mod = {
	accesslog_open, NULL, NULL
};
