/** alphahttpd: receive data
2022, Simon Zolin */

#include <http/client.h>

static int ahrecv_open(alphahttpd_client *c)
{
	return AHFILTER_FWD;
}

static void ahrecv_close(alphahttpd_client *c)
{
	cl_timer_stop(c, &c->recv.timer);
}

static void ahreq_read_expired(alphahttpd_client *c)
{
	cl_dbglog(c, "receive timeout");
	c->si->cl_destroy(c);
}

static int ahrecv_read(alphahttpd_client *c)
{
	if (c->resp_done)
		return AHFILTER_DONE;

	if (c->req_unprocessed_data) {
		return AHFILTER_FWD;
	}

	if (c->req.buf.cap == 0) {
		if (NULL == ffvec_alloc(&c->req.buf, c->conf->receive.buf_size, 1)) {
			cl_syswarnlog(c, "no memory");
			return AHFILTER_ERR;
		}
	}

	int r = ffsock_recv_async(c->sk, c->req.buf.ptr + c->req.buf.len, c->req.buf.cap - c->req.buf.len, cl_kev_r(c));
	if (r < 0) {
		if (fferr_last() == FFSOCK_EINPROGRESS) {
			cl_timer(c, &c->recv.timer, c->conf->receive.timeout_sec, ahreq_read_expired, c);
			cl_async(c);
			return AHFILTER_ASYNC;
		}
		cl_dbglog(c, "ffsock_recv: %E", fferr_last());
		return AHFILTER_ERR;
	}

	cl_dbglog(c, "ffsock_recv: %u", r);

	if (r == 0) {
		if (c->req.buf.len != 0)
			cl_warnlog(c, "peer closed connection before finishing request");
		return AHFILTER_FIN;
	}

	c->req.buf.len += r;
	c->recv.transferred += r;
	cl_timer_stop(c, &c->recv.timer);
	return AHFILTER_FWD;
}

const struct alphahttpd_filter alphahttpd_filter_receive = {
	ahrecv_open, ahrecv_close, ahrecv_read
};
