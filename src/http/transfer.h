/** alphahttpd: data transfer filter
2022, Simon Zolin */

static int trans_open(struct client *c)
{
	if (c->resp.content_length == (ffuint64)-1) {
		c->resp_connection_keepalive = 0;
		return CHAIN_SKIP;
	}
	c->conlen = c->resp.content_length;
	return CHAIN_FWD;
}

static void trans_close(struct client *c)
{
}

static int trans_process(struct client *c)
{
	if (c->chain_back)
		return CHAIN_BACK;

	ffsize n = ffmin64(c->input.len, c->conlen);
	ffstr_set(&c->output, c->input.ptr, n);
	c->conlen -= n;
	if (c->conlen == 0)
		return CHAIN_DONE;
	return CHAIN_FWD;
}

static const struct ahd_mod trans_mod = {
	trans_open, trans_close, trans_process
};
