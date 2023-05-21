/** alphahttpd: prepare HTTP response
2022, Simon Zolin */

#include <http/client.h>

static int ahresp_open(alphahttpd_client *c)
{
	if (NULL == ffvec_alloc(&c->resp.buf, c->conf->response.buf_size, 1)) {
		cl_syswarnlog(c, "no memory");
		return AHFILTER_ERR;
	}
	return AHFILTER_FWD;
}

static void ahresp_close(alphahttpd_client *c)
{
	ffvec_free(&c->resp.buf);
	ffstr_free(&c->resp.last_modified);
}

static int ahresp_process(alphahttpd_client *c)
{
	char *d = (char*)c->resp.buf.ptr, *end = (char*)c->resp.buf.ptr + c->resp.buf.cap - 2;

	int r = http_resp_write(d, end - d, c->resp.code, c->resp.msg);
	if (r < 0) {
		cl_warnlog(c, "http_resp_write");
		return AHFILTER_FIN;
	}
	d += r;

	if (c->resp.content_length != (ffuint64)-1) {
		d += _ffs_copycz(d, end - d, "Content-Length: ");
		d += ffs_fromint(c->resp.content_length, d, end - d, 0);
		d += _ffs_copycz(d, end - d, "\r\n");
	}

	ffstr val;
	if (c->resp.location.len)
		d += http_hdr_write(d, end - d, FFSTR_Z("Location"), c->resp.location);

	if (c->resp.last_modified.len)
		d += http_hdr_write(d, end - d, FFSTR_Z("Last-Modified"), c->resp.last_modified);

	if (c->resp.content_type.len)
		d += http_hdr_write(d, end - d, FFSTR_Z("Content-Type"), c->resp.content_type);

	if (c->conf->response.server_name.len)
		d += http_hdr_write(d, end - d, FFSTR_Z("Server"), c->conf->response.server_name);

	ffstr_setz(&val, "keep-alive");
	if (!c->resp_connection_keepalive)
		ffstr_setz(&val, "close");
	d += http_hdr_write(d, end - d, FFSTR_Z("Connection"), val);

	*d++ = '\r';
	*d++ = '\n';
	c->resp.buf.len = d - (char*)c->resp.buf.ptr;

	cl_dbglog(c, "response: %S", &c->resp.buf);

	ffiovec_set(&c->send.iov[0], c->resp.buf.ptr, c->resp.buf.len);
	if (!c->req_method_head)
		ffiovec_set(&c->send.iov[1], c->input.ptr, c->input.len);
	else
		c->resp_done = 1;
	c->input.len = 0;
	c->send.iov_n = 2;
	return AHFILTER_DONE;
}

const struct alphahttpd_filter alphahttpd_filter_response = {
	ahresp_open, ahresp_close, ahresp_process
};
