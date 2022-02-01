/** alphahttpd: index document
2022, Simon Zolin */

static int index_open(struct client *c)
{
	if (c->resp_err
		|| c->req.unescaped_path.ptr[c->req.unescaped_path.len-1] != '/')
		return CHAIN_SKIP;

	// ".../" -> ".../index.html"
	ffsize cap = c->req.unescaped_path.len + 1 + ahd_conf->index_filename.len;
	c->req.unescaped_path.ptr = ffmem_realloc(c->req.unescaped_path.ptr, cap);
	if (c->req.unescaped_path.ptr == NULL) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return CHAIN_SKIP;
	}
	ffstr_addchar(&c->req.unescaped_path, -1, '/');
	ffstr_add2(&c->req.unescaped_path, -1, &ahd_conf->index_filename);
	return CHAIN_SKIP;
}

static const struct ahd_mod index_mod = {
	index_open, NULL, NULL
};
