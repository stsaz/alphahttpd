/** alphahttpd: client interface
2023, Simon Zolin */

#pragma once
#include <FFOS/error.h>
#include <alphahttpd.h>
#include <util/range.h>
#include <util/http1.h>
#include <util/http1-status.h>
#include <FFOS/dir.h>
#include <FFOS/timerqueue.h>
#include <ffbase/time.h>
#include <ffbase/vector.h>

#ifdef FF_WIN
typedef unsigned int uint;
#endif

/** Client-context logger */

#define cl_errlog(c, ...) \
	c->log(c->opaque, ALPHAHTTPD_LOG_ERR, c->id, __VA_ARGS__)

#define cl_syswarnlog(c, ...) \
	c->log(c->opaque, ALPHAHTTPD_LOG_SYSWARN, c->id, __VA_ARGS__)

#define cl_warnlog(c, ...) \
	c->log(c->opaque, ALPHAHTTPD_LOG_WARN, c->id, __VA_ARGS__)

#define cl_verblog(c, ...) \
do { \
	if (c->log_level >= ALPHAHTTPD_LOG_VERBOSE) \
		c->log(c->opaque, ALPHAHTTPD_LOG_VERBOSE, c->id, __VA_ARGS__); \
} while (0)

#define cl_dbglog(c, ...) \
do { \
	if (c->log_level >= ALPHAHTTPD_LOG_DEBUG) \
		c->log(c->opaque, ALPHAHTTPD_LOG_DEBUG, c->id, __VA_ARGS__); \
} while (0)

#define cl_extralog(c, ...)
#ifdef ALPH_ENABLE_LOG_EXTRA
	#undef cl_extralog
	#define cl_extralog(c, ...) \
	do { \
		if (c->log_level >= ALPHAHTTPD_LOG_DEBUG) \
			c->log(c->opaque, ALPHAHTTPD_LOG_EXTRA, c->id, __VA_ARGS__); \
	} while (0)
#endif


typedef void (*ahd_kev_func)(void *obj);
struct ahd_kev {
	ahd_kev_func rhandler, whandler;
	union {
		ffkq_task rtask;
		ffkq_task_accept rtask_accept;
	};
	ffkq_task wtask;
	uint side;
	void *obj;
	struct ahd_kev *next_kev;
	struct ffkcall kcall;
};

typedef fftimerqueue_node ahd_timer;

/** Server runtime interface */
struct ahd_server {
	struct alphahttpd_conf *conf;
	fftime (*date)(alphahttpd *srv, ffstr *dts);
	int (*kq_attach)(alphahttpd *srv, ffsock sk, struct ahd_kev *kev, void *obj);
	void (*timer)(alphahttpd *srv, ahd_timer *tmr, int interval_msec, fftimerqueue_func func, void *param);
	void (*cl_destroy)(alphahttpd_client *c);
};

/** Client (connection) context */
struct alphahttpd_client {
	struct ahd_kev *kev;
	ffsock sk;
	alphahttpd *srv;
	struct ahd_server *si;
	struct alphahttpd_conf *conf;
	void *opaque;
	uint log_level;
	void (*log)(void *opaque, uint level, const char *id, const char *fmt, ...);

	ffbyte peer_ip[16];
	ffushort peer_port;
	ffushort keep_alive_n;
	uint send_init :1;
	uint kq_attached :1;
	uint req_unprocessed_data :1;
	char id[12]; // "*ID"

	// next data is cleared before each keep-alive/pipeline request

	ffuint64 start_time_msec;

	struct {
		ffuint64 transferred;
		ahd_timer timer;
	} recv;

	struct {
		range16 full, line, method, path, querystr, host, if_modified_since;
		ffstr unescaped_path;
		ffvec buf;
	} req;

	struct {
		const struct alphahttpd_virtdoc *vdoc;
	} vspace;

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

	struct {
		ffuint64 cont_len;
	} transfer;

	struct {
		uint code;
		ffuint64 content_length;
		ffstr msg, location, content_type;
		ffstr last_modified;
		ffvec buf;
	} resp;

	struct {
		ffiovec iov[3];
		uint iov_n;
		ahd_timer timer;
		ffuint64 transferred;
	} send;

	uint chain_back :1;
	uint req_method_head :1;
	uint resp_connection_keepalive :1;
	uint resp_err :1;
	uint resp_done :1;
	uint ka :1;

	uint imod;
	struct {
		uint opened :1;
		uint done :1;
	} mdata[16];
	ffstr input, output;
};

/** Set timer */
#define cl_timer(c, tmr, interval_sec, f, p) \
	c->si->timer(c->srv, tmr, interval_sec*1000, (void(*)(void*))f, p)

/** Disable timer */
#define cl_timer_stop(c, tmr) \
	c->si->timer(c->srv, tmr, 0, NULL, NULL)

/** Set error HTTP response status */
static inline void cl_resp_status(alphahttpd_client *c, enum HTTP_STATUS status)
{
	c->resp.code = http_status_code[status];
	if (c->resp.code == 400)
		c->resp_connection_keepalive = 0;
	ffstr_setz(&c->resp.msg, http_status_msg[status]);
	c->resp_err = 1;
}

/** Set success HTTP response status */
static inline void cl_resp_status_ok(alphahttpd_client *c, enum HTTP_STATUS status)
{
	c->resp.code = http_status_code[status];
	ffstr_setz(&c->resp.msg, http_status_msg[status]);
}

#define cl_kev_w(c)  &c->kev->wtask
#define cl_kev_r(c)  &c->kev->rtask
#define cl_kcq(c)  &c->kev->kcall
#define cl_kcq_active(c)  (c->kev->kcall.op != 0)

static inline int cl_async(alphahttpd_client *c)
{
	if (!c->kq_attached) {
		c->kq_attached = 1;
		if (0 != c->si->kq_attach(c->srv, c->sk, c->kev, c)) {
			c->si->cl_destroy(c);
			return -1;
		}
	}
	return 0;
}


enum AHFILTER_R {
	AHFILTER_DONE,
	AHFILTER_FWD,
	AHFILTER_BACK,
	AHFILTER_ASYNC,
	AHFILTER_SKIP,
	AHFILTER_ERR,
	AHFILTER_FIN,
};

/** A plugin implements this interface so it can be placed into HTTP request processing chain */
struct alphahttpd_filter {
	/**
	Return enum AHFILTER_R */
	int (*open)(alphahttpd_client *c);

	void (*close)(alphahttpd_client *c);

	/**
	Return enum AHFILTER_R */
	int (*process)(alphahttpd_client *c);
};
