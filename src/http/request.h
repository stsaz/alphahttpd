/** alphahttpd: parse HTTP request
2022, Simon Zolin */

#include <http/client.h>
#include <FFOS/path.h>
#include <FFOS/perf.h>

static int ahreq_parse(alphahttpd_client *c);

static int ahreq_open(alphahttpd_client *c)
{
	return AHFILTER_FWD;
}

static void ahreq_close(alphahttpd_client *c)
{
	if (c->ka) {
		// preserve pipelined data
		ffstr_erase_left((ffstr*)&c->req.buf, c->req.full.len);
		c->req_unprocessed_data = (c->req.buf.len != 0);
	} else {
		ffvec_free(&c->req.buf);
	}
	ffstr_free(&c->req.unescaped_path);
}

static int ahreq_read(alphahttpd_client *c)
{
	if (c->req_unprocessed_data) {
		c->req_unprocessed_data = 0;
	}

	if (0 == ahreq_parse(c)) {
		return AHFILTER_DONE;
	}

	if (c->req.buf.len == c->req.buf.cap) {
		cl_warnlog(c, "reached `read_buf_size` limit");
		return AHFILTER_ERR;
	}

	return AHFILTER_BACK;
}

/**
Return 0 if request is complete
 >0 if need more data */
static int ahreq_parse(alphahttpd_client *c)
{
	char *buf = (char*)c->req.buf.ptr;
	ffstr req = FFSTR_INITSTR(&c->req.buf), method, url, proto;
	int r, ka = 0;

	if (c->start_time_msec == 0) {
		fftime t = c->si->date(c->srv, NULL);
		c->start_time_msec = t.sec*1000 + t.nsec/1000000;
	}

	fftime t_begin;
	if (c->log_level >= ALPHAHTTPD_LOG_DEBUG)
		t_begin = fftime_monotonic();

	r = http_req_parse(req, &method, &url, &proto);
	if (r == 0)
		return 1;
	else if (r < 0) {
		cl_resp_status(c, HTTP_400_BAD_REQUEST);
		return 0;
	}

	range16_set(&c->req.line, 0, r-1);
	if (req.ptr[r-2] == '\r')
		c->req.line.len--;
	ffstr_shift(&req, r);

	ffstr name = {}, val = {};
	for (;;) {
		r = http_hdr_parse(req, &name, &val);
		if (r == 0) {
			return 1;
		} else if (r < 0) {
			cl_warnlog(c, "bad header");
			// cl_dbglog(c, "full request data: %S", &c->req.buf);
			cl_resp_status(c, HTTP_400_BAD_REQUEST);
			return 0;
		}
		ffstr_shift(&req, r);

		if (r <= 2)
			break;

		if (ffstr_ieqcz(&name, "Host") && c->req.host.len == 0) {
			range16_set(&c->req.host, val.ptr - buf, val.len);

		} else if (ffstr_ieqcz(&name, "Connection")) {
			if (ffstr_ieqcz(&val, "keep-alive"))
				ka = 1;
			else if (ffstr_ieqcz(&val, "close"))
				ka = -1;

		} else if (ffstr_ieqcz(&name, "If-Modified-Since")) {
			range16_set(&c->req.if_modified_since, val.ptr - buf, val.len);
		}
	}

	cl_dbglog(c, "request: [%u] %*s", (int)(req.ptr - buf), req.ptr - buf, buf);

	range16_set(&c->req.full, 0, req.ptr - buf);

	c->resp_connection_keepalive = (proto.ptr[7] == '1');
	if (ka > 0)
		c->resp_connection_keepalive = 1;
	else if (ka < 0)
		c->resp_connection_keepalive = 0;

	if (proto.ptr[7] == '1' && c->req.host.len == 0) {
		cl_warnlog(c, "no host");
		cl_resp_status(c, HTTP_400_BAD_REQUEST);
		return 0;
	}

	struct httpurl_parts parts = {};
	httpurl_split(&parts, url);

	range16_set(&c->req.method, method.ptr - buf, method.len);
	range16_set(&c->req.path, parts.path.ptr - buf, parts.path.len);
	range16_set(&c->req.querystr, parts.query.ptr - buf, parts.query.len);

	r = httpurl_unescape(NULL, 0, parts.path);
	if (NULL == ffstr_alloc(&c->req.unescaped_path, r)) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return 0;
	}
	r = httpurl_unescape(c->req.unescaped_path.ptr, r, parts.path);
	if (r <= 0) {
		cl_warnlog(c, "httpurl_unescape");
		cl_resp_status(c, HTTP_400_BAD_REQUEST);
		return 0;
	}

	r = ffpath_normalize(c->req.unescaped_path.ptr, r, c->req.unescaped_path.ptr, r, FFPATH_SLASH_ONLY | FFPATH_NO_DISK_LETTER);
	if (r <= 0) {
		cl_warnlog(c, "ffpath_normalize");
		cl_resp_status(c, HTTP_400_BAD_REQUEST);
		return 0;
	}
	c->req.unescaped_path.len = r;

	if (c->log_level >= ALPHAHTTPD_LOG_DEBUG) {
		fftime t_end = fftime_monotonic();
		fftime_sub(&t_end, &t_begin);
		cl_dbglog(c, "request parsing time: %uus", t_end.sec*1000000 + t_end.nsec/1000);
	}

	return 0;
}

const struct alphahttpd_filter alphahttpd_filter_request = {
	ahreq_open, ahreq_close, ahreq_read
};
