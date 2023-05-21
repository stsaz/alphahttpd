/** alphahttpd: index document
2022, Simon Zolin */

#include <http/client.h>
#include <FFOS/file.h>

static int index_open(alphahttpd_client *c)
{
	if (c->resp_err || c->resp.code != 0
		|| *ffstr_last(&c->req.unescaped_path) != '/')
		return AHFILTER_SKIP;

	return AHFILTER_FWD;
}

void index_close(alphahttpd_client *c)
{
	ffvec_free(&c->index.buf);
}

/** ".../" -> ".../index.html" */
int index_process(alphahttpd_client *c)
{
	if (0 == ffvec_addfmt(&c->index.buf, "%S%S%S%Z"
		, &c->conf->fs.www, &c->req.unescaped_path, &c->conf->fs.index_filename)) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return AHFILTER_DONE;
	}
	const char *fn = c->index.buf.ptr;

	fffd fd;
	if (FFFILE_NULL == (fd = fffile_open(fn, FFFILE_READONLY | FFFILE_NOATIME))) {
		if (!fferr_notexist(fferr_last())) {
			cl_syswarnlog(c, "index: fffile_open: %s", fn);
		}
		return AHFILTER_DONE;
	}
	cl_dbglog(c, "index: found %s", fn);
	fffile_close(fd);
	ffvec_free(&c->index.buf);

	ffsize cap = c->req.unescaped_path.len;
	if (0 == ffstr_growadd2(&c->req.unescaped_path, &cap, &c->conf->fs.index_filename)) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return AHFILTER_DONE;
	}
	return AHFILTER_DONE;
}

const struct alphahttpd_filter alphahttpd_filter_index = {
	index_open, index_close, index_process
};
