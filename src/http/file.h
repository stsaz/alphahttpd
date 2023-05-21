/** alphahttpd: static file filter/module
2022, Simon Zolin */

#include <http/client.h>
#include <util/ltconf.h>
#include <FFOS/kcall.h>
#include <ffbase/map.h>

static int file_open(alphahttpd_client *c)
{
	if (c->resp_err || c->resp.code != 0)
		return AHFILTER_SKIP;

	c->file.f = FFFILE_NULL;

	ffstr method = range16_tostr(&c->req.method, c->req.buf.ptr);
	if (!(ffstr_eqz(&method, "GET")
			|| (c->req_method_head = ffstr_eqz(&method, "HEAD")))) {
		cl_resp_status(c, HTTP_405_METHOD_NOT_ALLOWED);
		return AHFILTER_SKIP;
	}

	if (c->conf->fs.www.len + c->req.unescaped_path.len + 1 > c->conf->fs.file_buf_size) {
		cl_warnlog(c, "too small file buffer");
		return AHFILTER_ERR;
	}
	if (NULL == ffvec_alloc(&c->file.buf, c->conf->fs.file_buf_size, 1)) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return AHFILTER_SKIP;
	}
	ffstr *fn = (ffstr*)&c->file.buf;
	ffstr_add2(fn, -1, &c->conf->fs.www);
	ffstr_add2(fn, -1, &c->req.unescaped_path);
	fn->ptr[fn->len] = '\0';

	return AHFILTER_FWD;
}

static void file_close(alphahttpd_client *c)
{
	if (c->file.f != FFFILE_NULL) {
		fffile_close(c->file.f);
		c->file.f = FFFILE_NULL;
	}
	ffvec_free(&c->file.buf);
}

static int handle_redirect(alphahttpd_client *c, const fffileinfo *fi)
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

static int mtime(alphahttpd_client *c, const fffileinfo *fi)
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

static int ctmap_keyeq(void *opaque, const void *key, ffsize keylen, void *val)
{
	alphahttpd_client *c = opaque;
	ffuint hi = (ffuint64)val >> 32;
	ffstr k = range16_tostr((range16*)&hi, c->conf->fs.content_types_data);
	return ffstr_eq(&k, key, keylen);
}

void alphahttpd_filter_file_init(struct alphahttpd_conf *conf, ffstr content_types)
{
	conf->fs.content_types_data = content_types.ptr;

	ffmap_init(&conf->fs.content_types_map, ctmap_keyeq);
	ffmap_alloc(&conf->fs.content_types_map, 16);
	ffstr in = content_types, out, ct = {};
	struct ltconf ltc = {};

	for (;;) {
		int r = ltconf_read(&ltc, &in, &out);

		switch (r) {
		case LTCONF_KEY:
			ct = out;
			break;
		case LTCONF_VAL:
		case LTCONF_VAL_NEXT: {
			if (out.len > 4)
				return;
			range16 k, v;
			range16_set(&k, out.ptr - content_types.ptr, out.len);
			range16_set(&v, ct.ptr - content_types.ptr, ct.len);
			ffuint64 val = FFINT_JOIN64(*(uint*)&k, *(uint*)&v);
			ffmap_add(&conf->fs.content_types_map, out.ptr, out.len, (void*)val);
			break;
		}

		case LTCONF_MORE:
		case LTCONF_ERROR:
		default:
			return;
		}
	}
}

void alphahttpd_filter_file_uninit(struct alphahttpd_conf *conf)
{
	if (conf == NULL) return;

	ffmap_free(&conf->fs.content_types_map);
	ffmem_free(conf->fs.content_types_data);
}

static void content_type(alphahttpd_client *c)
{
	ffstr x;
	int r = ffpath_splitname(c->file.buf.ptr, c->file.buf.len, NULL, &x);
	if (r <= 0 || ((char*)c->file.buf.ptr)[r] == '/' || x.len > 4)
		goto unknown;

	char lx[4];
	ffs_lower(lx, 4, x.ptr, x.len);
	void *val = ffmap_find(&c->conf->fs.content_types_map, lx, x.len, c);
	if (val == NULL)
		goto unknown;

	ffuint lo = (ffuint64)val;
	ffstr v = range16_tostr((range16*)&lo, c->conf->fs.content_types_data);
	ffstr_setstr(&c->resp.content_type, &v);
	return;

unknown:
	ffstr_setz(&c->resp.content_type, "application/octet-stream");
}

static int f_open(alphahttpd_client *c)
{
	const char *fname = c->file.buf.ptr;
	if (cl_kcq_active(c))
		cl_dbglog(c, "fffile_open: completed");

	if (FFFILE_NULL == (c->file.f = fffile_open_async(fname, FFFILE_READONLY | FFFILE_NOATIME, cl_kcq(c)))) {
		if (fferr_notexist(fferr_last())) {
			cl_dbglog(c, "fffile_open: %s: not found", fname);
			cl_resp_status(c, HTTP_404_NOT_FOUND);
			return AHFILTER_DONE;
		} else if (fferr_last() == FFKCALL_EINPROGRESS) {
			cl_dbglog(c, "fffile_open: %s: in progress", fname);
			return AHFILTER_ASYNC;
		}
		cl_syswarnlog(c, "fffile_open: %s", fname);
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		return AHFILTER_DONE;
	}

	return AHFILTER_FWD;
}

static int f_info(alphahttpd_client *c)
{
	if (cl_kcq_active(c))
		cl_dbglog(c, "fffile_info: completed");

	if (0 != fffile_info_async(c->file.f, &c->file.info, cl_kcq(c))) {
		if (fferr_last() == FFKCALL_EINPROGRESS) {
			cl_dbglog(c, "fffile_info: in progress");
			return AHFILTER_ASYNC;
		}
		cl_syswarnlog(c, "fffile_info: %s", c->file.buf.ptr);
		cl_resp_status(c, HTTP_403_FORBIDDEN);
		return AHFILTER_DONE;
	}

	if (0 != handle_redirect(c, &c->file.info))
		return AHFILTER_DONE;
	if (0 != mtime(c, &c->file.info))
		return AHFILTER_DONE;
	content_type(c);

	c->resp.content_length = fffileinfo_size(&c->file.info);
	cl_resp_status_ok(c, HTTP_200_OK);

	if (c->req_method_head) {
		c->resp_done = 1;
		return AHFILTER_DONE;
	}

	return AHFILTER_FWD;
}

static int file_process(alphahttpd_client *c)
{
	ffssize r;
	switch (c->file.state) {
	case 0:
		if (AHFILTER_FWD != (r = f_open(c)))
			return r;
		c->file.state = 1;
		// fallthrough
	case 1:
		if (AHFILTER_FWD != (r = f_info(c)))
			return r;
		c->file.state = 2;
	}

	if (cl_kcq_active(c))
		cl_dbglog(c, "fffile_read: completed");

	r = fffile_read_async(c->file.f, c->file.buf.ptr, c->file.buf.cap, cl_kcq(c));
	if (r < 0) {
		if (fferr_last() == FFKCALL_EINPROGRESS) {
			cl_dbglog(c, "fffile_read: in progress");
			return AHFILTER_ASYNC;
		}
		cl_syswarnlog(c, "fffile_read");
		return AHFILTER_ERR;
	} else if (r == 0) {
		c->resp_done = 1;
		return AHFILTER_DONE;
	}
	c->file.buf.len = r;
	ffstr_setstr(&c->output, &c->file.buf);
	return AHFILTER_FWD;
}

const struct alphahttpd_filter alphahttpd_filter_file = {
	file_open, file_close, file_process
};
