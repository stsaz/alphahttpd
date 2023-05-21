/** alphahttpd: data transfer filter
2022, Simon Zolin */

#include <http/client.h>

static int ahtrans_open(alphahttpd_client *c)
{
	if (c->resp.content_length == (ffuint64)-1) {
		c->resp_connection_keepalive = 0;
		return AHFILTER_SKIP;
	}
	c->transfer.cont_len = c->resp.content_length;
	return AHFILTER_FWD;
}

static void ahtrans_close(alphahttpd_client *c)
{
}

static int ahtrans_process(alphahttpd_client *c)
{
	if (c->chain_back)
		return AHFILTER_BACK;

	ffsize n = ffmin64(c->input.len, c->transfer.cont_len);
	ffstr_set(&c->output, c->input.ptr, n);
	c->transfer.cont_len -= n;
	if (c->transfer.cont_len == 0)
		return AHFILTER_DONE;
	return AHFILTER_FWD;
}

const struct alphahttpd_filter alphahttpd_filter_transfer = {
	ahtrans_open, ahtrans_close, ahtrans_process
};
