/** alphahttpd: error document
2022, Simon Zolin */

static int err_open(struct client *c)
{
	if (!c->resp_err)
		return CHAIN_SKIP;
	return CHAIN_FWD;
}

static void err_close(struct client *c)
{
}

static int err_process(struct client *c)
{
	ffstr_setz(&c->resp.content_type, "text/plain");
	c->resp.content_length = c->resp.msg.len;
	c->resp_done = 1;
	ffstr_setstr(&c->output, &c->resp.msg);
	return CHAIN_DONE;
}

static const struct ahd_mod err_mod = {
	err_open, err_close, err_process
};
