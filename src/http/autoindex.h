/** alphahttpd: show directory contents
2022, Simon Zolin */

#include <http/client.h>
#include <FFOS/dirscan.h>

static int autoindex_open(alphahttpd_client *c)
{
	if (c->resp_err
		|| *ffstr_last(&c->req.unescaped_path) != '/')
		return AHFILTER_SKIP;

	return AHFILTER_FWD;
}

void autoindex_close(alphahttpd_client *c)
{
	ffvec_free(&c->autoindex.path);
	ffvec_free(&c->autoindex.buf);
}

int autoindex_process(alphahttpd_client *c)
{
	ffdirscan ds = {};
	ffvec namebuf = {};

	if (0 == ffvec_addfmt(&c->autoindex.path, "%S%S%Z"
		, &c->conf->fs.www, &c->req.unescaped_path)) {
		cl_errlog(c, "no memory");
		cl_resp_status(c, HTTP_500_INTERNAL_SERVER_ERROR);
		goto end;
	}
	const char *path = c->autoindex.path.ptr;

	if (0 != ffdirscan_open(&ds, path, 0)) {
		cl_syswarnlog(c, "ffdirscan_open: %s", path);
		int rc = HTTP_403_FORBIDDEN;
		if (fferr_notexist(fferr_last()))
			rc = HTTP_404_NOT_FOUND;
		cl_resp_status(c, rc);
		goto end;
	}

	ffvec_addfmt(&c->autoindex.buf,
		"<html>\n"
		"<head>\n"
			"<meta charset=\"utf-8\">\n"
			"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
			"<title>Index of %S</title>\n"
		"</head>\n"
		"<body>\n"
			"<h1>Index of %S</h1>\n"
			"<pre>\n"
				"<a href=\"%S..\">..</a>\n"
		, &c->req.unescaped_path, &c->req.unescaped_path, &c->req.unescaped_path);

	ffvec_addstr(&namebuf, &c->req.unescaped_path);

	for (;;) {
		const char *fn = ffdirscan_next(&ds);
		if (fn == NULL)
			break;

		namebuf.len = c->req.unescaped_path.len;
		ffvec_add(&namebuf, fn, ffsz_len(fn)+1, 1);
		ffvec_addfmt(&c->autoindex.buf, "<a href=\"%s\">%s</a>\n"
			, namebuf.ptr, fn);
	}

	ffvec_addfmt(&c->autoindex.buf, "</pre></body></html>");

	c->resp.content_length = c->autoindex.buf.len;
	cl_resp_status_ok(c, HTTP_200_OK);
	ffstr_setstr(&c->output, &c->autoindex.buf);
	c->resp_done = 1;

end:
	ffvec_free(&namebuf);
	ffdirscan_close(&ds);
	return AHFILTER_DONE;
}

const struct alphahttpd_filter alphahttpd_filter_autoindex = {
	autoindex_open, autoindex_close, autoindex_process
};
