/** alphahttpd: index document
2022, Simon Zolin */

static int index_open(struct client *c)
{
	if (c->resp_err
		|| *ffstr_last(&c->req.unescaped_path) != '/')
		return CHAIN_SKIP;

	return CHAIN_FWD;
}

void index_close(struct client *c)
{
	ffvec_free(&c->index.buf);
}

/** ".../" -> ".../index.html" */
int index_process(struct client *c)
{
	if (0 == ffvec_addfmt(&c->index.buf, "%S%S%S%Z"
		, &ahd_conf->www, &c->req.unescaped_path, &ahd_conf->index_filename)) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return CHAIN_DONE;
	}
	const char *fn = c->index.buf.ptr;

	fffd fd;
	if (FFFILE_NULL == (fd = fffile_open(fn, FFFILE_READONLY | FFFILE_NOATIME))) {
		if (!fferr_notexist(fferr_last())) {
			cl_syswarnlog(c, "index: fffile_open: %s", fn);
		}
		cl_resp_status(c, HTTP_403_FORBIDDEN);
		return CHAIN_DONE;
	}
	cl_dbglog(c, "index: found %s", fn);
	fffile_close(fd);
	ffvec_free(&c->index.buf);

	ffsize cap = c->req.unescaped_path.len;
	if (0 == ffstr_growadd2(&c->req.unescaped_path, &cap, &ahd_conf->index_filename)) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return CHAIN_DONE;
	}
	return CHAIN_DONE;
}

static const struct ahd_mod index_mod = {
	index_open, index_close, index_process
};
