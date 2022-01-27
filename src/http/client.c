/** alphahttpd: client
2022, Simon Zolin */

/* Modules chain when serving a file:
request -> index -> file/error <-> content-length <-> response -> access-log
*/

#include <alphahttpd.h>
#include <util/http1.h>
#include <util/ipaddr.h>
#include <util/range.h>
#include <FFOS/socket.h>
#include <ffbase/vector.h>

enum {
	CHAIN_DONE,
	CHAIN_FWD,
	CHAIN_BACK,
	CHAIN_ASYNC,
	CHAIN_ERR,
	CHAIN_FIN,
};
struct client;
struct ahd_mod {
	int (*open)(struct client *c);
	void (*close)(struct client *c);
	int (*process)(struct client *c);
};
static const struct ahd_mod req_mod;
static const struct ahd_mod index_mod;
static const struct ahd_mod file_mod;
static const struct ahd_mod err_mod;
static const struct ahd_mod trans_mod;
static const struct ahd_mod resp_mod;
static const struct ahd_mod accesslog_mod;
static const struct ahd_mod* const mods[] = {
	&req_mod,
	&index_mod,
	&file_mod,
	&err_mod,
	&trans_mod,
	&resp_mod,
	&accesslog_mod,
};

struct client {
	struct ahd_kev *kev;
	struct server *srv;
	ffsock sk;
	uint log_level;

	ffbyte peer_ip[16];
	ushort peer_port;
	ushort keep_alive_n;
	unsigned send_init :1;
	unsigned kq_attached :1;

	// next data is cleared before each keep-alive/pipeline request

	char id[12]; // "*ID"
	ffuint64 start_time_msec;

	struct {
		range16 full, line, method, path, querystr, host, if_modified_since;
		ffstr unescaped_path;
		ffvec buf;
		ffuint64 transferred;
	} req;

	struct {
		fffd f;
		ffvec buf;
	} file;

	ffuint64 conlen;

	struct {
		uint code;
		ffuint64 content_length;
		ffstr msg, location;
		ffstr last_modified;
		ffvec buf;
		ffuint64 transferred;
	} resp;

	struct {
		ffiovec iov[3];
		uint iov_n;
	} send;

	unsigned chain_back :1;
	unsigned req_unprocessed_data :1;
	unsigned resp_connection_keepalive :1;
	unsigned resp_err :1;
	unsigned resp_done :1;

	uint imod;
	struct {
		uint opened :1;
		uint done :1;
	} mdata[8];
	ffstr input, output;
};

static void cl_init(struct client *c);
static void cl_mods_close(struct client *c);
static void cl_chain_process(struct client *c);
static void cl_kq_attach(struct client *c);

enum HTTP_STATUS {
	HTTP_200_OK,
	HTTP_206_PARTIAL,

	HTTP_301_MOVED_PERMANENTLY,
	HTTP_302_FOUND,
	HTTP_304_NOT_MODIFIED,

	HTTP_400_BAD_REQUEST,
	HTTP_403_FORBIDDEN,
	HTTP_404_NOT_FOUND,
	HTTP_405_METHOD_NOT_ALLOWED,
	HTTP_413_REQUEST_ENTITY_TOO_LARGE,
	HTTP_415_UNSUPPORTED_MEDIA_TYPE,
	HTTP_416_REQUESTED_RANGE_NOT_SATISFIABLE,

	HTTP_500_INTERNAL_SERVER_ERROR,
	HTTP_501_NOT_IMPLEMENTED,
	HTTP_502_BAD_GATEWAY,
	HTTP_504_GATEWAY_TIMEOUT,

	_HTTP_STATUS_END,
};
static const ffushort http_status_code[] = {
	200,
	206,

	301,
	302,
	304,

	400,
	403,
	404,
	405,
	413,
	415,
	416,

	500,
	501,
	502,
	504,
};
static const char http_status_msg[][32] = {
	"OK",
	"Partial",

	"Moved Permanently",
	"Found",
	"Not Modified",

	"Bad Request",
	"Forbidden",
	"Not Found",
	"Method Not Allowed",
	"Request Entity Too Large",
	"Unsupported Media Type",
	"Requested Range Not Satisfiable",

	"Internal Server Error",
	"Not Implemented",
	"Bad Gateway",
	"Gateway Timeout",
};
static void cl_resp_status(struct client *c, enum HTTP_STATUS status);
static void cl_resp_status_ok(struct client *c, enum HTTP_STATUS status);

#define cl_errlog(c, ...) \
	ahd_log(LOG_ERR, c->id, __VA_ARGS__)

#define cl_syswarnlog(c, ...) \
	ahd_log(LOG_SYSWARN, c->id, __VA_ARGS__)

#define cl_warnlog(c, ...) \
	ahd_log(LOG_WARN, c->id, __VA_ARGS__)

#define cl_verblog(c, ...) \
do { \
	if (c->log_level >= LOG_VERB) \
		ahd_log(LOG_VERB, c->id, __VA_ARGS__); \
} while (0)

#define cl_dbglog(c, ...) \
do { \
	if (c->log_level >= LOG_DBG) \
		ahd_log(LOG_DBG, c->id, __VA_ARGS__); \
} while (0)

#include <http/request.h>
#include <http/index.h>
#include <http/file.h>
#include <http/error.h>
#include <http/transfer.h>
#include <http/response.h>
#include <http/access-log.h>

void cl_start(struct ahd_kev *kev, ffsock csock, const ffsockaddr *peer, struct server *s)
{
	struct client *c = ffmem_alloc(sizeof(struct client) + ahd_conf->read_buf_size + ahd_conf->write_buf_size);
	if (c == NULL) {
		ahd_log(LOG_ERR, NULL, "no memory");
		return;
	}
	ffmem_zero_obj(c);
	c->sk = csock;
	c->srv = s;
	c->kev = kev;
	c->log_level = ahd_conf->log_level;

	uint port = 0;
	ffslice ip = ffsockaddr_ip_port(peer, &port);
	ffmem_copy(c->peer_ip, ip.ptr, ip.len);
	c->peer_port = port;

	char buf[FFIP6_STRLEN];
	int r = ffip46_tostr((void*)c->peer_ip, buf, sizeof(buf));
	cl_verblog(c, "new client connection: %*s:%u"
		, (ffsize)r, buf, c->peer_port);

	cl_init(c);
	cl_chain_process(c);
}

static void cl_init(struct client *c)
{
	c->id[0] = '*';
	uint id = sv_req_id_next(c->srv);
	int r = ffs_fromint(id, c->id+1, sizeof(c->id)-1, 0);
	c->id[r+1] = '\0';

	c->req.buf.ptr = c+1;
	c->req.buf.cap = ahd_conf->read_buf_size;
	c->resp.buf.ptr = (char*)c->req.buf.ptr + ahd_conf->read_buf_size;
	c->resp.buf.cap = ahd_conf->write_buf_size;

	c->resp.content_length = (ffuint64)-1;
}

static void cl_destroy(struct client *c)
{
	cl_dbglog(c, "closing client connection");
	ffsock_close(c->sk);

	cl_mods_close(c);
	sv_conn_fin(c->srv, c->kev);
	ffmem_free(c);
}

static void cl_mods_close(struct client *c)
{
	for (int i = 0;  i != FF_COUNT(mods);  i++) {
		if (c->mdata[i].opened)
			mods[i]->close(c);
	}
}

static void cl_reset(struct client *c)
{
	cl_mods_close(c);

	uint req_len = c->req.full.len, buf_len = c->req.buf.len;
	ffmem_zero(c->id, sizeof(*c) - FF_OFF(struct client, id));
	cl_init(c);

	// preserve pipelined data
	c->req.buf.len = buf_len;
	ffstr_erase_left((ffstr*)&c->req.buf, req_len);
	c->req_unprocessed_data = (c->req.buf.len != 0);
}

static int cl_keepalive(struct client *c)
{
	if (!c->resp_connection_keepalive)
		return -1;
	c->keep_alive_n++;
	if (c->keep_alive_n == ahd_conf->max_keep_alive_reqs)
		return -1;
	cl_reset(c);
	return 0;
}

static void cl_chain_process(struct client *c)
{
	int i = c->imod, r;
	for (;;) {
		if (!c->mdata[i].opened) {
			cl_dbglog(c, "opening module %u", i);
			r = mods[i]->open(c);
			cl_dbglog(c, "  module %u returned: %u", i, r);
			if (r == CHAIN_DONE || r == CHAIN_ERR) {
				c->mdata[i].done = 1;
				c->output = c->input;
			} else {
				c->mdata[i].opened = 1;
			}
		}

		if (!c->mdata[i].done) {
			cl_dbglog(c, "calling module %u input:%L", i, c->input.len);
			r = mods[i]->process(c);
			cl_dbglog(c, "  module %u returned: %u  output:%L", i, r, c->output.len);
		} else {
			r = (!c->chain_back) ? CHAIN_FWD : CHAIN_BACK;
		}

		switch (r) {
		case CHAIN_DONE:
			c->mdata[i].done = 1;
			// fallthrough
		case CHAIN_FWD:
			c->input = c->output;
			ffstr_null(&c->output);
			c->chain_back = 0;
			i++;
			if (i == FF_COUNT(mods)) {
				if (0 != cl_keepalive(c))
					goto end;
				i = 0;
			}
			break;

		case CHAIN_BACK:
			if (i == 0) {
				FF_ASSERT(0);
				goto end;
			}
			ffstr_null(&c->input);
			i--;
			c->chain_back = 1;
			break;

		case CHAIN_ASYNC:
			c->imod = i;
			return;

		case CHAIN_ERR:
			c->mdata[i].done = 1;
			goto end;

		case CHAIN_FIN:
			goto end;

		default:
			FF_ASSERT(0);
		}
	}

end:
	cl_destroy(c);
}

static void cl_kq_attach(struct client *c)
{
	c->kq_attached = 1;
	if (0 != sv_kq_attach(c->srv, c->sk, c->kev, (void*)c))
		cl_destroy(c);
}

static void cl_resp_status(struct client *c, enum HTTP_STATUS status)
{
	c->resp.code = http_status_code[status];
	if (c->resp.code == 400)
		c->resp_connection_keepalive = 0;
	ffstr_setz(&c->resp.msg, http_status_msg[status]);
	c->resp_err = 1;
}

static void cl_resp_status_ok(struct client *c, enum HTTP_STATUS status)
{
	c->resp.code = http_status_code[status];
	ffstr_setz(&c->resp.msg, http_status_msg[status]);
}
