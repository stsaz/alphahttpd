/** alphahttpd: send data
2022, Simon Zolin */

#include <http/client.h>

static int ahsend_open(alphahttpd_client *c)
{
	return AHFILTER_FWD;
}

static void ahsend_close(alphahttpd_client *c)
{
	cl_timer_stop(c, &c->send.timer);
}

static void ahsend_expired(alphahttpd_client *c)
{
	cl_dbglog(c, "send timeout");
	c->si->cl_destroy(c);
}

static int ahsend_process(alphahttpd_client *c)
{
	if (!c->send_init) {
		c->send_init = 1;
		if (c->conf->send.tcp_nodelay && 0 != ffsock_setopt(c->sk, IPPROTO_TCP, TCP_NODELAY, 1)) {
			cl_syswarnlog(c, "socket setopt(TCP_NODELAY)");
		}
	}

	if (c->input.len != 0) {
		ffiovec_set(&c->send.iov[0], c->input.ptr, c->input.len);
		c->send.iov_n = 1;
		c->input.len = 0;
	}

	while (c->send.iov_n != 0) {
		int r = ffsock_sendv_async(c->sk, c->send.iov, c->send.iov_n, cl_kev_w(c));
		if (r < 0) {
			if (fferr_last() == FFSOCK_EINPROGRESS) {
				cl_timer(c, &c->send.timer, c->conf->send.timeout_sec, ahsend_expired, c);
				cl_async(c);
				return AHFILTER_ASYNC;
			}
			cl_syswarnlog(c, "socket writev");
			return AHFILTER_ERR;
		}

		cl_dbglog(c, "ffsock_sendv: %u", r);
		c->send.transferred += r;

		if (ffiovec_array_shift(c->send.iov, c->send.iov_n, r) == 0) {
			c->send.iov_n = 0;
			break;
		}
	}

	cl_timer_stop(c, &c->send.timer);
	if (c->resp_done)
		return AHFILTER_DONE;
	return AHFILTER_BACK;
}

const struct alphahttpd_filter alphahttpd_filter_send = {
	ahsend_open, ahsend_close, ahsend_process
};
