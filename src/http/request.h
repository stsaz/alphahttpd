/** alphahttpd: receive and parse HTTP request
2022, Simon Zolin */

#include <FFOS/path.h>
#include <FFOS/perf.h>

static int req_parse(struct client *c);

static int req_open(struct client *c)
{
	return CHAIN_FWD;
}

static void req_close(struct client *c)
{
	ffstr_free(&c->req.unescaped_path);
	sv_timer(c->srv, &c->req.timer, 0, NULL, NULL);
}

static void req_read_expired(struct client *c)
{
	cl_dbglog(c, "receive timeout");
	cl_destroy(c);
}

static int req_read(struct client *c)
{
	if (c->kev->rhandler != NULL) {
		c->kev->rhandler = NULL;
		cl_chain_process(c);
		return -1;
	}

	if (c->req_unprocessed_data) {
		c->req_unprocessed_data = 0;
		c->req.transferred = c->req.buf.len;
		if (0 == req_parse(c))
			return CHAIN_FWD;
	}

	for (;;) {
		int r = ffsock_recv(c->sk, c->req.buf.ptr + c->req.buf.len, c->req.buf.cap - c->req.buf.len, 0);
		if (r < 0) {
			if (fferr_again(fferr_last())) {
				c->kev->rhandler = (ahd_kev_func)(void*)req_read;
				if (!c->kq_attached)
					cl_kq_attach(c);
				sv_timer(c->srv, &c->req.timer, ahd_conf->read_timeout_sec*1000, (fftimerqueue_func)req_read_expired, c);
				return CHAIN_ASYNC;
			}
			cl_syswarnlog(c, "ffsock_recv");
			return CHAIN_ERR;
		}

		cl_dbglog(c, "ffsock_recv: %u", r);

		if (r == 0) {
			if (c->req.buf.len != 0)
				cl_warnlog(c, "peer closed connection before finishing request");
			return CHAIN_FIN;
		}

		c->req.buf.len += r;
		c->req.transferred += r;

		if (0 == req_parse(c)) {
			sv_timer(c->srv, &c->req.timer, 0, NULL, NULL);
			return CHAIN_FWD;
		}

		if (c->req.buf.len == c->req.buf.cap) {
			cl_warnlog(c, "reached `read_buf_size` limit");
			return CHAIN_ERR;
		}
	}
}

static int crlf_skip(ffstr *d)
{
	if (d->len >= 2 && d->ptr[0] == '\r' && d->ptr[1] == '\n') {
		ffstr_shift(d, 2);
		return 1;
	} else if (d->len >= 1 && d->ptr[0] == '\n') {
		ffstr_shift(d, 1);
		return 1;
	}
	return 0;
}

/**
Return 0 if request is complete
 >0 if need more data */
static int req_parse(struct client *c)
{
	char *buf = (char*)c->req.buf.ptr;
	ffstr req = FFSTR_INITSTR(&c->req.buf), method, url, proto;
	int r, ka = 0;

	if (c->start_time_msec == 0) {
		fftime t = sv_date(c->srv, NULL);
		c->start_time_msec = t.sec*1000 + t.nsec/1000000;
	}

	fftime t_begin;
	if (c->log_level >= LOG_DBG)
		t_begin = fftime_monotonic();

	r = ffhttp_req_parse(req, &method, &url, &proto);
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

	for (;;) {
		ffstr name, val;
		r = ffhttp_hdr_parse(req, &name, &val);
		if (r == 0) {
			return 1;
		} else if (r < 0) {
			if (crlf_skip(&req))
				break;
			cl_warnlog(c, "bad header");
			// cl_dbglog(c, "full request data: %S", &c->req.buf);
			cl_resp_status(c, HTTP_400_BAD_REQUEST);
			return 0;
		}
		ffstr_shift(&req, r);

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

	struct ffhttpurl_parts parts = {};
	ffhttpurl_split(&parts, url);

	r = ffhttpurl_unescape(NULL, 0, parts.path);
	if (NULL == ffstr_alloc(&c->req.unescaped_path, r)) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return 0;
	}
	r = ffhttpurl_unescape(c->req.unescaped_path.ptr, r, parts.path);
	if (r <= 0) {
		cl_warnlog(c, "ffhttpurl_unescape");
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

	cl_dbglog(c, "request: [%u] %*s", (int)(req.ptr - buf), req.ptr - buf, buf);
	range16_set(&c->req.method, method.ptr - buf, method.len);
	range16_set(&c->req.path, parts.path.ptr - buf, parts.path.len);
	range16_set(&c->req.querystr, parts.query.ptr - buf, parts.query.len);

	if (c->log_level >= LOG_DBG) {
		fftime t_end = fftime_monotonic();
		fftime_sub(&t_end, &t_begin);
		cl_dbglog(c, "request parsing time: %uus", t_end.sec*1000000 + t_end.nsec/1000);
	}

	return 0;
}

static const struct ahd_mod req_mod = {
	req_open, req_close, req_read
};
