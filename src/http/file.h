/** alphahttpd: static file filter/module
2022, Simon Zolin */

#include <FFOS/file.h>

static void file_close(struct client *c);

static int handle_redirect(struct client *c, const fffileinfo *fi)
{
	if (!fffile_isdir(fffileinfo_attr(fi)))
		return 0;

	cl_dbglog(c, "handle_redirect: %s", c->file.buf.ptr);
	cl_resp_status(c, HTTP_301_MOVED_PERMANENTLY);

	ffstr host = range16_tostr(&c->req.host, c->req.buf.ptr);
	ffstr path = range16_tostr(&c->req.path, c->req.buf.ptr);
	// TODO some bad clients may set Host header value without specifying the non-standard port
	c->file.buf.len = ffs_format_r0(c->file.buf.ptr, c->file.buf.cap, "http://%S%S/"
		, &host, &path);
	ffstr_setstr(&c->resp.location, &c->file.buf);
	return -1;
}

static int mtime(struct client *c, const fffileinfo *fi)
{
	fftime mt = fffileinfo_mtime(fi);
	mt.sec += FFTIME_1970_SECONDS;
	ffdatetime dt;
	fftime_split1(&dt, &mt);
	int r = FFS_LEN("Wed, dd Sep yyyy hh:mm:ss GMT")+1;
	if (NULL == ffstr_alloc(&c->resp.last_modified, r)) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return -1;
	}
	c->resp.last_modified.len = fftime_tostr1(&dt, c->resp.last_modified.ptr, r, FFTIME_WDMY);

	if (c->req.if_modified_since.len != 0) {
		ffstr ims = range16_tostr(&c->req.if_modified_since, c->req.buf.ptr);
		if (ffstr_eq2(&c->resp.last_modified, &ims)) {
			cl_resp_status(c, HTTP_304_NOT_MODIFIED);
			return -1;
		}
	}
	return 0;
}

static int file_open(struct client *c)
{
	if (c->resp_err)
		return CHAIN_DONE;

	c->file.f = FFFILE_NULL;

	ffstr method = range16_tostr(&c->req.method, c->req.buf.ptr);
	if (!ffstr_eqz(&method, "GET")) {
		cl_resp_status(c, HTTP_405_METHOD_NOT_ALLOWED);
		goto fail;
	}

	if (ahd_conf->www.len + c->req.unescaped_path.len + 1 > ahd_conf->file_buf_size) {
		cl_warnlog(c, "too small file buffer");
		goto err;
	}
	if (NULL == ffvec_alloc(&c->file.buf, ahd_conf->file_buf_size, 1)) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return -1;
	}
	ffstr_add2((ffstr*)&c->file.buf, c->file.buf.cap, &ahd_conf->www);
	ffstr_add2((ffstr*)&c->file.buf, c->file.buf.cap, &c->req.unescaped_path);
	ffstr_addchar((ffstr*)&c->file.buf, c->file.buf.cap, '\0');

	const char *fname = c->file.buf.ptr;
	if (FFFILE_NULL == (c->file.f = fffile_open(fname, FFFILE_READONLY | FFFILE_NOATIME))) {
		if (fferr_notexist(fferr_last())) {
			cl_dbglog(c, "fffile_open: %s: not found", fname);
			cl_resp_status(c, HTTP_404_NOT_FOUND);
			goto fail;
		}
		cl_syswarnlog(c, "fffile_open: %s", fname);
		goto err;
	}

	fffileinfo fi;
	if (0 != fffile_info(c->file.f, &fi)) {
		cl_syswarnlog(c, "fffile_info: %s", fname);
		cl_resp_status(c, HTTP_403_FORBIDDEN);
		goto fail;
	}

	if (0 != handle_redirect(c, &fi))
		goto fail;
	if (0 != mtime(c, &fi))
		goto fail;

	c->resp.content_length = fffileinfo_size(&fi);
	cl_resp_status_ok(c, HTTP_200_OK);
	return CHAIN_FWD;

err:
	file_close(c);
	return CHAIN_ERR;

fail:
	file_close(c);
	return CHAIN_DONE;
}

static void file_close(struct client *c)
{
	fffile_close(c->file.f);
	ffvec_free(&c->file.buf);
}

static int file_process(struct client *c)
{
	ffssize r = fffile_read(c->file.f, c->file.buf.ptr, c->file.buf.cap);
	if (r < 0) {
		cl_syswarnlog(c, "fffile_read");
		return CHAIN_ERR;
	} else if (r == 0) {
		c->resp_done = 1;
		return CHAIN_DONE;
	}
	c->file.buf.len = r;
	ffstr_setstr(&c->output, &c->file.buf);
	return CHAIN_FWD;
}

static const struct ahd_mod file_mod = {
	file_open, file_close, file_process
};
