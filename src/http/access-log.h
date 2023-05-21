/** alphahttpd: write access log
2022, Simon Zolin */

#include <http/client.h>
#include <util/ipaddr.h>
#include <FFOS/std.h>

/* CLIENT_IP REQ_TOTAL "METHOD PATH VER" RESP_TOTAL "VER CODE MSG" REALTIME */
static int accesslog_open(alphahttpd_client *c)
{
	ffstr dts;
	fftime end_time = c->si->date(c->srv, &dts);
	uint tms = end_time.sec*1000 + end_time.nsec/1000000 - c->start_time_msec;

	ffstr req_line;
	req_line = range16_tostr(&c->req.line, c->req.buf.ptr);

	ffuint cap = 500 + req_line.len;
	if (NULL == ffstr_alloc(&c->acclog_buf, cap)) {
		cl_warnlog(c, "no memory");
		return AHFILTER_SKIP;
	}
	char *d = c->acclog_buf.ptr, *end = d + cap - 1;
	d += ffip46_tostr((void*)c->peer_ip, d, end - d);
	*d++ = '\t';
	d += ffs_format_r0(d, end - d, "%S \"%S\" %u %U %U %ums\n"
		, &dts
		, &req_line, (int)c->resp.code
		, c->recv.transferred, c->send.transferred
		, tms);
	c->acclog_buf.len = d - c->acclog_buf.ptr;
	return AHFILTER_FWD;
}

static void accesslog_close(alphahttpd_client *c)
{
	ffstr_free(&c->acclog_buf);
}

static int accesslog_process(alphahttpd_client *c)
{
	if (cl_kcq_active(c))
		cl_dbglog(c, "fffile_write: completed");

	int r = fffile_write_async(ffstderr, c->acclog_buf.ptr, c->acclog_buf.len, cl_kcq(c));
	if (r < 0) {
		if (fferr_last() == FFKCALL_EINPROGRESS) {
			cl_dbglog(c, "fffile_write: in progress");
			return AHFILTER_ASYNC;
		}
		cl_syswarnlog(c, "fffile_write");
		return AHFILTER_ERR;
	}

	return AHFILTER_DONE;
}

const struct alphahttpd_filter alphahttpd_filter_accesslog = {
	accesslog_open, accesslog_close, accesslog_process
};
