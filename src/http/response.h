/** alphahttpd: prepare and send HTTP response
2022, Simon Zolin */

static int resp_open(struct client *c)
{
	if (NULL == ffvec_alloc(&c->resp.buf, ahd_conf->write_buf_size, 1)) {
		cl_syswarnlog(c, "no memory");
		return CHAIN_ERR;
	}
	return CHAIN_FWD;
}

static void resp_close(struct client *c)
{
	ffvec_free(&c->resp.buf);
	ffstr_free(&c->resp.last_modified);
	sv_timer_stop(c->srv, &c->resp.timer);
}

static int resp_prepare(struct client *c);
static int resp_send(struct client *c);

static int resp_process(struct client *c)
{
	if (c->resp.buf.len == 0)
		if (0 != resp_prepare(c))
			return CHAIN_FIN;
	return resp_send(c);
}

static int resp_prepare(struct client *c)
{
	char *d = (char*)c->resp.buf.ptr, *end = (char*)c->resp.buf.ptr + c->resp.buf.cap - 2;

	int r = http_resp_write(d, end - d, c->resp.code, c->resp.msg);
	if (r < 0) {
		cl_warnlog(c, "http_resp_write");
		return -1;
	}
	d += r;

	if (c->resp.content_length != (ffuint64)-1) {
		d += _ffs_copycz(d, end - d, "Content-Length: ");
		d += ffs_fromint(c->resp.content_length, d, end - d, 0);
		d += _ffs_copycz(d, end - d, "\r\n");
	}

	ffstr name, val;
	if (c->resp.location.len != 0) {
		ffstr_setz(&name, "Location");
		ffstr_setstr(&val, &c->resp.location);
		d += http_hdr_write(d, end - d, name, val);
	}

	if (c->resp.last_modified.len != 0) {
		ffstr_setz(&name, "Last-Modified");
		ffstr_setstr(&val, &c->resp.last_modified);
		d += http_hdr_write(d, end - d, name, val);
	}

	if (c->resp.content_type.len != 0) {
		ffstr_setz(&name, "Content-Type");
		ffstr_setstr(&val, &c->resp.content_type);
		d += http_hdr_write(d, end - d, name, val);
	}

	d += _ffs_copycz(d, end - d, "Server: alphahttpd\r\n");

	ffstr_setz(&name, "Connection");
	if (c->resp_connection_keepalive)
		ffstr_setz(&val, "keep-alive");
	else
		ffstr_setz(&val, "close");
	d += http_hdr_write(d, end - d, name, val);

	*d++ = '\r';
	*d++ = '\n';
	c->resp.buf.len = d - (char*)c->resp.buf.ptr;

	cl_dbglog(c, "response: %S", &c->resp.buf);

	ffiovec_set(&c->send.iov[0], c->resp.buf.ptr, c->resp.buf.len);
	if (!c->req_method_head)
		ffiovec_set(&c->send.iov[1], c->input.ptr, c->input.len);
	else
		c->resp_done = 1;
	c->input.len = 0;
	c->send.iov_n = 2;
	return 0;
}

static void resp_send_expired(struct client *c)
{
	cl_dbglog(c, "send timeout");
	cl_destroy(c);
}

static int resp_send(struct client *c)
{
	if (c->kev->whandler != NULL) {
		c->kev->whandler = NULL;
		cl_chain_process(c);
		return -1;
	}

	if (!c->send_init) {
		c->send_init = 1;
		if (ahd_conf->tcp_nodelay && 0 != ffsock_setopt(c->sk, IPPROTO_TCP, TCP_NODELAY, 1)) {
			cl_syswarnlog(c, "socket setopt(TCP_NODELAY)");
		}
	}

	if (c->input.len != 0) {
		ffiovec_set(&c->send.iov[0], c->input.ptr, c->input.len);
		c->send.iov_n = 1;
		c->input.len = 0;
	}

	while (c->send.iov_n != 0) {
		int r = ffsock_sendv_async(c->sk, c->send.iov, c->send.iov_n, &c->kev->wtask);
		if (r < 0) {
			if (fferr_last() == FFSOCK_EINPROGRESS) {
				c->kev->whandler = (ahd_kev_func)(void*)resp_send;
				if (!c->kq_attached)
					cl_kq_attach(c);
				sv_timer(c->srv, &c->resp.timer, ahd_conf->write_timeout_sec*1000, (fftimerqueue_func)resp_send_expired, c);
				return CHAIN_ASYNC;
			}
			cl_syswarnlog(c, "socket writev");
			return CHAIN_ERR;
		}

		cl_dbglog(c, "ffsock_sendv: %u", r);
		c->resp.transferred += r;

		if (ffiovec_array_shift(c->send.iov, c->send.iov_n, r) == 0) {
			c->send.iov_n = 0;
			break;
		}
	}

	sv_timer_stop(c->srv, &c->resp.timer);
	if (c->resp_done)
		return CHAIN_DONE;
	return CHAIN_BACK;
}

static const struct ahd_mod resp_mod = {
	resp_open, resp_close, resp_process
};
