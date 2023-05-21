/** alphahttpd: error document
2022, Simon Zolin */

#include <http/client.h>

static int aherr_open(alphahttpd_client *c)
{
	if (!c->resp_err)
		return AHFILTER_SKIP;
	return AHFILTER_FWD;
}

static void aherr_close(alphahttpd_client *c)
{
}

static int aherr_process(alphahttpd_client *c)
{
	ffstr_setz(&c->resp.content_type, "text/plain");
	c->resp.content_length = c->resp.msg.len;
	c->resp_done = 1;
	ffstr_setstr(&c->output, &c->resp.msg);
	return AHFILTER_DONE;
}

const struct alphahttpd_filter alphahttpd_filter_error = {
	aherr_open, aherr_close, aherr_process
};
