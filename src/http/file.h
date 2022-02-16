/** alphahttpd: static file filter/module
2022, Simon Zolin */

#include <util/ltconf.h>
#include <ffbase/map.h>

static int file_open(struct client *c)
{
	if (c->resp_err)
		return CHAIN_SKIP;

	c->file.f = FFFILE_NULL;

	ffstr method = range16_tostr(&c->req.method, c->req.buf.ptr);
	if (!(ffstr_eqz(&method, "GET")
			|| (c->req_method_head = ffstr_eqz(&method, "HEAD")))) {
		cl_resp_status(c, HTTP_405_METHOD_NOT_ALLOWED);
		return CHAIN_SKIP;
	}

	if (ahd_conf->www.len + c->req.unescaped_path.len + 1 > ahd_conf->file_buf_size) {
		cl_warnlog(c, "too small file buffer");
		return CHAIN_ERR;
	}
	if (NULL == ffvec_alloc(&c->file.buf, ahd_conf->file_buf_size, 1)) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return CHAIN_SKIP;
	}
	ffstr *fn = (ffstr*)&c->file.buf;
	ffstr_add2(fn, -1, &ahd_conf->www);
	ffstr_add2(fn, -1, &c->req.unescaped_path);
	fn->ptr[fn->len] = '\0';

	return CHAIN_FWD;
}

static void file_close(struct client *c)
{
	if (c->file.f != FFFILE_NULL) {
		fffile_close(c->file.f);
	}
	ffvec_free(&c->file.buf);
}

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

static ffmap content_types_map;
static const char content_types_data[] = "\
image/gif	gif\n\
image/jpeg	jpg\n\
image/png	png\n\
image/svg+xml	svg\n\
image/webp	webp\n\
text/css	css\n\
text/html	htm html\n\
text/plain	txt\n\
";

static int ctmap_keyeq(void *opaque, const void *key, ffsize keylen, void *val)
{
	ffuint hi = (ffuint64)val >> 32;
	ffstr k = range16_tostr((range16*)&hi, content_types_data);
	return ffstr_eq(&k, key, keylen);
}

void content_types_init()
{
	ffmap_init(&content_types_map, ctmap_keyeq);
	ffmap_alloc(&content_types_map, 9);
	ffstr in = FFSTR_INITZ(content_types_data), out, ct = {};
	struct ltconf conf = {};

	for (;;) {
		int r = ltconf_read(&conf, &in, &out);

		switch (r) {
		case LTCONF_KEY:
			ct = out;
			break;
		case LTCONF_VAL: {
			if (out.len > 4)
				return;
			range16 k, v;
			range16_set(&k, out.ptr - content_types_data, out.len);
			range16_set(&v, ct.ptr - content_types_data, ct.len);
			ffuint64 val = FFINT_JOIN64(*(uint*)&k, *(uint*)&v);
			ffmap_add(&content_types_map, out.ptr, out.len, (void*)val);
			break;
		}

		case LTCONF_MORE:
		case LTCONF_ERROR:
		default:
			return;
		}
	}
}

static void content_type(struct client *c)
{
	ffstr x;
	int r = ffpath_splitname(c->file.buf.ptr, c->file.buf.len, NULL, &x);
	if (r <= 0 || ((char*)c->file.buf.ptr)[r] == '/' || x.len > 4)
		goto unknown;

	char lx[4];
	ffs_lower(lx, 4, x.ptr, x.len);
	void *val = ffmap_find(&content_types_map, lx, x.len, NULL);
	if (val == NULL)
		goto unknown;

	ffuint lo = (ffuint64)val;
	ffstr v = range16_tostr((range16*)&lo, content_types_data);
	ffstr_setstr(&c->resp.content_type, &v);
	return;

unknown:
	ffstr_setz(&c->resp.content_type, "application/octet-stream");
}

static int f_open(struct client *c)
{
	const char *fname = c->file.buf.ptr;
	if (c->kev->kcall.handler != NULL) {
		c->kev->kcall.handler = NULL;
		cl_dbglog(c, "fffile_open: completed");
		cl_chain_process(c);
		return -1;
	}

	if (FFFILE_NULL == (c->file.f = fffile_open_async(fname, FFFILE_READONLY | FFFILE_NOATIME, &c->kev->kcall))) {
		if (fferr_notexist(fferr_last())) {
			cl_dbglog(c, "fffile_open: %s: not found", fname);
			cl_resp_status(c, HTTP_404_NOT_FOUND);
			return CHAIN_DONE;
		} else if (fferr_last() == EINPROGRESS) {
			cl_dbglog(c, "fffile_open: %s: in progress", fname);
			c->kev->kcall.handler = (void*)f_open;
			return CHAIN_ASYNC;
		}
		cl_syswarnlog(c, "fffile_open: %s", fname);
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return CHAIN_DONE;
	}

	return CHAIN_FWD;
}

static int f_info(struct client *c)
{
	if (c->kev->kcall.handler != NULL) {
		c->kev->kcall.handler = NULL;
		cl_dbglog(c, "fffile_info: completed");
		cl_chain_process(c);
		return -1;
	}

	if (0 != fffile_info_async(c->file.f, &c->file.info, &c->kev->kcall)) {
		if (fferr_last() == EINPROGRESS) {
			cl_dbglog(c, "fffile_info: in progress");
			c->kev->kcall.handler = (void*)f_info;
			return CHAIN_ASYNC;
		}
		cl_syswarnlog(c, "fffile_info: %s", c->file.buf.ptr);
		cl_resp_status(c, HTTP_403_FORBIDDEN);
		return CHAIN_DONE;
	}

	if (0 != handle_redirect(c, &c->file.info))
		return CHAIN_DONE;
	if (0 != mtime(c, &c->file.info))
		return CHAIN_DONE;
	content_type(c);

	c->resp.content_length = fffileinfo_size(&c->file.info);
	cl_resp_status_ok(c, HTTP_200_OK);

	if (c->req_method_head) {
		c->resp_done = 1;
		return CHAIN_DONE;
	}

	return CHAIN_FWD;
}

static int file_process(struct client *c)
{
	ffssize r;
	switch (c->file.state) {
	case 0:
		if (CHAIN_FWD != (r = f_open(c)))
			return r;
		c->file.state = 1;
		// fallthrough
	case 1:
		if (CHAIN_FWD != (r = f_info(c)))
			return r;
		c->file.state = 2;
	}

	if (c->kev->kcall.handler != NULL) {
		c->kev->kcall.handler = NULL;
		cl_dbglog(c, "fffile_read: completed");
		cl_chain_process(c);
		return -1;
	}

	r = fffile_read_async(c->file.f, c->file.buf.ptr, c->file.buf.cap, &c->kev->kcall);
	if (r < 0) {
		if (fferr_last() == EINPROGRESS) {
			cl_dbglog(c, "fffile_read: in progress");
			c->kev->kcall.handler = (void*)file_process;
			return CHAIN_ASYNC;
		}
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
