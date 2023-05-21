/** alphahttpd: virtual directory
2023, Simon Zolin */

#include <http/client.h>
#include <ffbase/murmurhash3.h>

static int map_keyeq_func(void *opaque, const void *key, ffsize keylen, void *val);

static uint hash_compute(ffstr path, ffstr method)
{
	uint hash = murmurhash3(path.ptr, path.len, 0x12345678);
	return murmurhash3(method.ptr, method.len, hash);
}

int alphahttpd_filter_virtspace_init(struct alphahttpd_conf *conf, const struct alphahttpd_virtdoc *docs)
{
	ffmap *m = &conf->virtspace.map;
	ffmap_init(m, map_keyeq_func);

	for (uint i = 0;  ;  i++) {
		const struct alphahttpd_virtdoc *d = &docs[i];
		if (d->path == NULL)
			break;

		uint hash = hash_compute(FFSTR_Z(d->path), FFSTR_Z(d->method));
		if (0 != ffmap_add_hash(m, hash, (void*)d))
			return -1;
	}
	return 0;
}

void alphahttpd_filter_virtspace_uninit(struct alphahttpd_conf *conf)
{
	if (conf == NULL) return;

	ffmap_free(&conf->virtspace.map);
}

static int ahvspc_open(alphahttpd_client *c)
{
	ffstr path = range16_tostr(&c->req.path, c->req.buf.ptr);
	ffstr method = range16_tostr(&c->req.method, c->req.buf.ptr);
	uint hash = hash_compute(path, method);
	if (NULL == (c->vspace.vdoc = ffmap_find_hash(&c->conf->virtspace.map, hash, c, 0, NULL)))
		return AHFILTER_SKIP;
	return AHFILTER_FWD;
}

static void ahvspc_close(alphahttpd_client *c)
{
}

static int map_keyeq_func(void *opaque, const void *key, ffsize keylen, void *val)
{
	const alphahttpd_client *c = key;
	const struct alphahttpd_virtdoc *d = val;
	ffstr path = range16_tostr(&c->req.path, c->req.buf.ptr);
	ffstr method = range16_tostr(&c->req.method, c->req.buf.ptr);
	if (ffstr_eqz(&path, d->path)
		&& ffstr_eqz(&method, d->method))
		return 1;
	return 0;
}

static int ahvspc_process(alphahttpd_client *c)
{
	c->vspace.vdoc->handler(c);

	if (c->resp.content_length == ~0ULL) {
		cl_resp_status_ok(c, HTTP_200_OK);
		c->resp.content_length = 0;
		c->resp_done = 1;
	}
	return AHFILTER_DONE;
}

const struct alphahttpd_filter alphahttpd_filter_virtspace = {
	ahvspc_open, ahvspc_close, ahvspc_process
};
