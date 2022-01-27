/** alphahttpd: error document
2022, Simon Zolin */

static int err_open(struct client *c)
{
	if (!c->resp_err)
		return CHAIN_DONE;
	return CHAIN_FWD;
}

static void err_close(struct client *c)
{
}

static int err_process(struct client *c)
{
	ffstr_setstr(&c->output, &c->resp.msg);
	c->resp.content_length = c->output.len;
	c->resp_done = 1;
	return CHAIN_DONE;
}

static const struct ahd_mod err_mod = {
	err_open, err_close, err_process
};
