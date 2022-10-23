/** alphahttpd: client
2022, Simon Zolin */

/* Modules chain when serving a file:
request -> index -> file/error <-> content-length <-> response -> access-log
*/

#include <alphahttpd.h>
#include <util/http1.h>
#include <util/http1-status.h>
#include <util/ipaddr.h>
#include <util/range.h>
#include <FFOS/socket.h>
#include <FFOS/file.h>
#include <ffbase/vector.h>

enum {
	CHAIN_DONE,
	CHAIN_FWD,
	CHAIN_BACK,
	CHAIN_ASYNC,
	CHAIN_SKIP,
	CHAIN_ERR,
	CHAIN_FIN,
};
static const char codestr[][12] = {
	"CHAIN_DONE",
	"CHAIN_FWD",
	"CHAIN_BACK",
	"CHAIN_ASYNC",
	"CHAIN_SKIP",
	"CHAIN_ERR",
	"CHAIN_FIN",
};

struct client;
struct ahd_mod {
	int (*open)(struct client *c);
	void (*close)(struct client *c);
	int (*process)(struct client *c);
};
static const struct ahd_mod req_mod;
static const struct ahd_mod index_mod;
static const struct ahd_mod autoindex_mod;
static const struct ahd_mod file_mod;
static const struct ahd_mod err_mod;
static const struct ahd_mod trans_mod;
static const struct ahd_mod resp_mod;
static const struct ahd_mod accesslog_mod;
static const struct ahd_mod* const mods[] = {
	&req_mod,
	&index_mod,
	&autoindex_mod,
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
	unsigned req_unprocessed_data :1;
	char id[12]; // "*ID"

	// next data is cleared before each keep-alive/pipeline request

	ffuint64 start_time_msec;

	struct {
		range16 full, line, method, path, querystr, host, if_modified_since;
		ffstr unescaped_path;
		ffvec buf;
		ffuint64 transferred;
		ahd_timer timer;
	} req;

	struct {
		ffvec buf;
	} index;

	struct {
		ffvec path;
		ffvec buf;
	} autoindex;

	struct {
		fffd f;
		ffvec buf;
		fffileinfo info;
		uint state;
	} file;

	ffstr acclog_buf;

	ffuint64 conlen;

	struct {
		uint code;
		ffuint64 content_length;
		ffstr msg, location, content_type;
		ffstr last_modified;
		ffvec buf;
		ffuint64 transferred;
		ahd_timer timer;
	} resp;

	struct {
		ffiovec iov[3];
		uint iov_n;
	} send;

	unsigned chain_back :1;
	unsigned req_method_head :1;
	unsigned resp_connection_keepalive :1;
	unsigned resp_err :1;
	unsigned resp_done :1;
	unsigned ka :1;

	uint imod;
	struct {
		uint opened :1;
		uint done :1;
	} mdata[FF_COUNT(mods)];
	ffstr input, output;
};

static void cl_init(struct client *c);
static void cl_chain_process(struct client *c);
static int cl_kq_attach(struct client *c);
static void cl_destroy(struct client *c);
static void cl_resp_status(struct client *c, enum HTTP_STATUS status);
static void cl_resp_status_ok(struct client *c, enum HTTP_STATUS status);

#define cl_errlog(c, ...) \
	ahd_log(c->srv, LOG_ERR, c->id, __VA_ARGS__)

#define cl_syswarnlog(c, ...) \
	ahd_log(c->srv, LOG_SYSWARN, c->id, __VA_ARGS__)

#define cl_warnlog(c, ...) \
	ahd_log(c->srv, LOG_WARN, c->id, __VA_ARGS__)

#define cl_verblog(c, ...) \
do { \
	if (c->log_level >= LOG_VERB) \
		ahd_log(c->srv, LOG_VERB, c->id, __VA_ARGS__); \
} while (0)

#define cl_dbglog(c, ...) \
do { \
	if (c->log_level >= LOG_DBG) \
		ahd_log(c->srv, LOG_DBG, c->id, __VA_ARGS__); \
} while (0)

#define cl_fulllog(c, ...)
#ifdef FF_DEBUG
	#undef cl_fulllog
	#define cl_fulllog(c, ...) cl_dbglog(c, __VA_ARGS__)
#endif

#include <http/request.h>
#include <http/index.h>
#include <http/autoindex.h>
#include <http/file.h>
#include <http/error.h>
#include <http/transfer.h>
#include <http/response.h>
#include <http/access-log.h>

void http_mods_init()
{
	content_types_init();
}

void cl_start(struct ahd_kev *kev, ffsock csock, const ffsockaddr *peer, struct server *s, uint conn_id)
{
	struct client *c = ffmem_alloc(sizeof(struct client));
	if (c == NULL) {
		ahd_log(s, LOG_ERR, NULL, "no memory");
		return;
	}
	ffmem_zero_obj(c);
	c->sk = csock;
	c->srv = s;
	c->kev = kev;
	c->log_level = ahd_conf->log_level;
	c->kev->kcall.param = c;

	c->id[0] = '*';
	int r = ffs_fromint(conn_id, c->id+1, sizeof(c->id)-1, 0);
	c->id[r+1] = '\0';

	uint port = 0;
	ffslice ip = ffsockaddr_ip_port(peer, &port);
	ffmem_copy(c->peer_ip, ip.ptr, ip.len);
	c->peer_port = port;

	if (c->log_level >= LOG_VERB) {
		char buf[FFIP6_STRLEN];
		r = ffip46_tostr((void*)c->peer_ip, buf, sizeof(buf));
		cl_verblog(c, "new client connection: %*s:%u"
			, (ffsize)r, buf, c->peer_port);
	}

#ifdef FF_WIN
	if (0 != cl_kq_attach(c))
		return;
#endif

	cl_init(c);
	cl_chain_process(c);
}

static void cl_init(struct client *c)
{
	c->resp.content_length = (ffuint64)-1;
}

static void cl_mods_close(struct client *c)
{
	for (uint i = 0;  i != FF_COUNT(mods);  i++) {
		if (c->mdata[i].opened) {
			cl_fulllog(c, "closing module %u", i);
			mods[i]->close(c);
		}
	}
}

static void cl_destroy(struct client *c)
{
	cl_verblog(c, "closing client connection");
	ffsock_close(c->sk);
	ffkcall_cancel(&c->kev->kcall);

	cl_mods_close(c);
	sv_conn_fin(c->srv, c->kev);
	ffmem_free(c);
}

static void cl_reset(struct client *c)
{
	c->ka = 1;
	cl_mods_close(c);

	ffvec rb = c->req.buf; // preserve pipelined data

	ffmem_zero(&c->start_time_msec, sizeof(*c) - FF_OFF(struct client, start_time_msec));
	ffkcall_cancel(&c->kev->kcall);
	cl_init(c);

	c->req.buf = rb;
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
		if (!c->mdata[i].opened && !c->mdata[i].done) {
			cl_fulllog(c, "opening module %u", i);
			r = mods[i]->open(c);
			(void)codestr;
			cl_fulllog(c, "  module %u returned: %s", i, codestr[r]);
			if (r == CHAIN_SKIP || r == CHAIN_ERR) {
				c->mdata[i].done = 1;
				c->output = c->input;
			} else {
				c->mdata[i].opened = 1;
			}
		}

		if (!c->mdata[i].done) {
			cl_fulllog(c, "calling module %u input:%L", i, c->input.len);
			r = mods[i]->process(c);
			cl_fulllog(c, "  module %u returned: %s  output:%L", i, codestr[r], c->output.len);
		} else {
			r = (!c->chain_back) ? CHAIN_FWD : CHAIN_BACK;
			c->output = c->input;
		}

		switch (r) {
		case CHAIN_SKIP:
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

static int cl_kq_attach(struct client *c)
{
	c->kq_attached = 1;
	if (0 != sv_kq_attach(c->srv, c->sk, c->kev, (void*)c)) {
		cl_destroy(c);
		return -1;
	}
	return 0;
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
