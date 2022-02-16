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

	ffuint cap = 500 + req_line.len;
	if (NULL == ffstr_alloc(&c->acclog_buf, cap)) {
		cl_warnlog(c, "no memory");
		return CHAIN_SKIP;
	}
	char *d = c->acclog_buf.ptr, *end = d + cap - 1;
	d += ffip46_tostr((void*)c->peer_ip, d, end - d);
	*d++ = '\t';
	d += ffs_format_r0(d, end - d, "%S \"%S\" %u %U %U %ums\n"
		, &dts
		, &req_line, (int)c->resp.code
		, c->req.transferred, c->resp.transferred
		, tms);
	c->acclog_buf.len = d - c->acclog_buf.ptr;
	return CHAIN_FWD;
}

static void accesslog_close(struct client *c)
{
	ffstr_free(&c->acclog_buf);
}

static int accesslog_process(struct client *c)
{
	if (c->kev->kcall.handler != NULL) {
		c->kev->kcall.handler = NULL;
		cl_dbglog(c, "fffile_write: completed");
		cl_chain_process(c);
		return -1;
	}

	int r = fffile_write_async(ffstderr, c->acclog_buf.ptr, c->acclog_buf.len, &c->kev->kcall);
	if (r < 0) {
		if (fferr_last() == EINPROGRESS) {
			cl_dbglog(c, "fffile_write: in progress");
			c->kev->kcall.handler = (void*)accesslog_process;
			return CHAIN_ASYNC;
		}
		cl_syswarnlog(c, "fffile_write");
		return CHAIN_ERR;
	}

	return CHAIN_DONE;
}

static const struct ahd_mod accesslog_mod = {
	accesslog_open, accesslog_close, accesslog_process
};
