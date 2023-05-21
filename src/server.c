/** alphahttpd: server
2022, Simon Zolin */

#include <http/client.h>
#include <util/ipaddr.h>
#include <FFOS/queue.h>
#include <FFOS/socket.h>
#include <FFOS/timer.h>
#include <FFOS/perf.h>
#include <FFOS/thread.h>

struct alphahttpd {
	struct alphahttpd_conf conf;
	struct ahd_server si;
	struct ahd_kev lsock_kev;
	// ffuint64 thd_id;
	ffsock lsock;
	ffkq kq;
	ffkq_event *kevents;
	struct ahd_kev *connections;
	uint connections_n;
	struct ahd_kev *reusable_connections_lifo;
	uint iconn, conn_num;
	ahd_timer tmr_fdlimit;
	struct ahd_kev post_kev;
	ffkq_postevent kqpost;
	uint worker_stop;
	uint sock_family;

	struct ffkcallqueue kcq;
	struct ahd_kev kcq_kev;

	fftimer timer;
	fftimerqueue timer_q;
	struct ahd_kev timer_kev;
	uint timer_now_ms;
	fftime date_now;
	char date_buf[FFS_LEN("0000-00-00T00:00:00.000")+1];
};

extern void cl_start(struct ahd_kev *kev, ffsock csock, const ffsockaddr *peer, uint conn_id, alphahttpd *srv, struct ahd_server *si);
extern void cl_destroy(alphahttpd_client *c);

static void sv_accept(alphahttpd *s);
static int sv_timer_start(alphahttpd *s);
static int sv_kq_attach(alphahttpd *s, ffsock sk, struct ahd_kev *kev, void *obj);
static void sv_timer(alphahttpd *s, ahd_timer *tmr, int interval_msec, fftimerqueue_func func, void *param);
fftime sv_date(alphahttpd *s, ffstr *dts);
static int sv_worker(alphahttpd *s);
static void kcq_onsignal(alphahttpd *s);

#define sv_sysfatallog(s, ...) \
	s->conf.log(s->conf.opaque, ALPHAHTTPD_LOG_SYSFATAL, NULL, __VA_ARGS__)

#define sv_syserrlog(s, ...) \
	s->conf.log(s->conf.opaque, ALPHAHTTPD_LOG_SYSERR, NULL, __VA_ARGS__)

#define sv_errlog(s, ...) \
	s->conf.log(s->conf.opaque, ALPHAHTTPD_LOG_ERR, NULL, __VA_ARGS__)

#define sv_warnlog(s, ...) \
	s->conf.log(s->conf.opaque, ALPHAHTTPD_LOG_WARN, NULL, __VA_ARGS__)

#define sv_verblog(s, ...) \
do { \
	if (s->conf.log_level >= ALPHAHTTPD_LOG_VERBOSE) \
		s->conf.log(s->conf.opaque, ALPHAHTTPD_LOG_VERBOSE, NULL, __VA_ARGS__); \
} while (0)

#define sv_dbglog(s, ...) \
do { \
	if (s->conf.log_level >= ALPHAHTTPD_LOG_DEBUG) \
		s->conf.log(s->conf.opaque, ALPHAHTTPD_LOG_DEBUG, NULL, __VA_ARGS__); \
} while (0)

#define sv_extralog(s, ...)
#ifdef ALPH_ENABLE_LOG_EXTRA
	#undef sv_extralog
	#define sv_extralog(s, ...) \
	do { \
		if (s->conf.log_level >= ALPHAHTTPD_LOG_DEBUG) \
			s->conf.log(s->conf.opaque, ALPHAHTTPD_LOG_EXTRA, NULL, __VA_ARGS__); \
	} while (0)
#endif

alphahttpd* alphahttpd_new()
{
	alphahttpd *s = ffmem_new(struct alphahttpd);
	s->kq = FFKQ_NULL;
	s->lsock = FFSOCK_NULL;
	s->timer = FFTIMER_NULL;
	return s;
}

static void sv_log(void *opaque, ffuint level, const char *id, const char *format, ...)
{}
static void sv_logv(void *opaque, ffuint level, const char *id, const char *format, va_list va)
{}

/** Initialize default config */
static void sv_conf_init(struct alphahttpd_conf *conf)
{
	ffmem_zero_obj(conf);
	conf->log_level = ALPHAHTTPD_LOG_INFO;
	conf->log = sv_log;
	conf->logv = sv_logv;

	static struct alphahttpd_address a[2];
	a[0].port = 80;
	conf->server.listen_addresses = a;
	conf->server.events_num = 1024;
	conf->server.fdlimit_timeout_sec = 10;
	conf->server.timer_interval_msec = 250;
	conf->server.max_connections = 10000;
	conf->server.conn_id_counter = &conf->server._conn_id_counter_default;

	conf->max_keep_alive_reqs = 100;

	conf->receive.buf_size = 4096;
	conf->receive.timeout_sec = 65;

	ffstr_setz(&conf->fs.index_filename, "index.html");
	conf->fs.file_buf_size = 16*1024;

	conf->response.buf_size = 4096;
	ffstr_setz(&conf->response.server_name, "alphahttpd");

	conf->send.tcp_nodelay = 1;
	conf->send.timeout_sec = 65;
}

int alphahttpd_conf(alphahttpd *s, struct alphahttpd_conf *conf)
{
	if (s == NULL) {
		sv_conf_init(conf);
		return 0;
	}

	s->conf = *conf;
	s->si.conf = &s->conf;
	s->si.kq_attach = sv_kq_attach;
	s->si.timer = sv_timer;
	s->si.date = sv_date;
	s->si.cl_destroy = cl_destroy;
	return 0;
}

void alphahttpd_stop(alphahttpd *s)
{
	if (s->worker_stop) return;

	sv_dbglog(s, "stopping kq worker");
	FFINT_WRITEONCE(s->worker_stop, 1);
	ffkq_post(s->kqpost, &s->post_kev);
}

static int lsock_prepare(alphahttpd *s)
{
	const struct alphahttpd_address *a = s->conf.server.listen_addresses;

	const void *ip4 = ffip6_tov4((void*)a->ip);
	s->sock_family = (ip4 != NULL) ? AF_INET : AF_INET6;
	if (FFSOCK_NULL == (s->lsock = ffsock_create_tcp(s->sock_family, FFSOCK_NONBLOCK))) {
		sv_sysfatallog(s, "ffsock_create_tcp");
		return -1;
	}

	ffsockaddr addr = {};
	if (ip4 != NULL) {
		ffsockaddr_set_ipv4(&addr, ip4, a->port);
	} else {
		ffsockaddr_set_ipv6(&addr, a->ip, a->port);

		// Allow clients to connect via IPv4
		if (ffip6_isany((void*)a->ip)
			&& 0 != ffsock_setopt(s->lsock, IPPROTO_IPV6, IPV6_V6ONLY, 0)) {
			sv_sysfatallog(s, "ffsock_setopt(IPV6_V6ONLY)");
			return -1;
		}
	}

#ifdef FF_UNIX
	// Allow several listening sockets to bind to the same address/port.
	// OS automatically distributes the load among the sockets.
	if (0 != ffsock_setopt(s->lsock, SOL_SOCKET, SO_REUSEPORT, 1)) {
		sv_sysfatallog(s, "ffsock_setopt(SO_REUSEPORT)");
		return -1;
	}
#endif

	if (0 != ffsock_bind(s->lsock, &addr)) {
		sv_sysfatallog(s, "socket bind");
		return -1;
	}

	if (0 != ffsock_listen(s->lsock, SOMAXCONN)) {
		sv_sysfatallog(s, "socket listen");
		return -1;
	}

	sv_verblog(s, "listening on %u", a->port);
	return 0;
}

static int kcq_init(alphahttpd *s)
{
	if (s->conf.kcq_set == NULL) return 0;

	s->conf.kcq_set(s->conf.opaque, &s->kcq);
	if (NULL == (s->kcq.cq = ffrq_alloc(s->connections_n))) {
		sv_sysfatallog(s, "ffrq_alloc");
		return -1;
	}
	if (s->conf.server.polling_mode) {
		s->kcq.kqpost = FFKQ_NULL;
		return 0;
	}

	s->kcq_kev.rhandler = (ahd_kev_func)kcq_onsignal;
	s->kcq_kev.obj = s;
	s->kcq_kev.rtask.active = 1;
	if (FFKQ_NULL == (s->kcq.kqpost = ffkq_post_attach(s->kq, &s->kcq_kev))) {
		sv_sysfatallog(s, "ffkq_post_attach");
		return -1;
	}
	s->kcq.kqpost_data = &s->kcq_kev;
	return 0;
}

int alphahttpd_run(alphahttpd *s)
{
	fftime_now(&s->date_now);
	s->date_now.sec += FFTIME_1970_SECONDS;

	// s->thd_id = ffthread_curid();

	s->connections_n = s->conf.server.max_connections;
	s->connections = (void*)ffmem_alloc(s->connections_n * sizeof(struct ahd_kev));
	s->kevents = (void*)ffmem_alloc(s->conf.server.events_num * sizeof(ffkq_event));
	if (s->connections == NULL || s->kevents == NULL) {
		sv_sysfatallog(s, "no memory");
		return -1;
	}
	ffmem_zero(s->connections, s->connections_n * sizeof(struct ahd_kev));

	if (FFKQ_NULL == (s->kq = ffkq_create())) {
		sv_sysfatallog(s, "ffkq_create");
		return -1;
	}
	if (FFKQ_NULL == (s->kqpost = ffkq_post_attach(s->kq, &s->post_kev))) {
		sv_sysfatallog(s, "ffkq_post_attach");
		return -1;
	}

	if (0 != lsock_prepare(s))
		return -1;

	s->lsock_kev.rhandler = (ahd_kev_func)sv_accept;
	s->lsock_kev.obj = s;
	if (0 != ffkq_attach_socket(s->kq, s->lsock, &s->lsock_kev, FFKQ_READ)) {
		sv_sysfatallog(s, "ffkq_attach_socket");
		return -1;
	}

	if (0 != sv_timer_start(s))
		return -1;

	if (0 != kcq_init(s))
		return -1;

	sv_accept(s);
	sv_worker(s);
	return 0;
}

void alphahttpd_free(alphahttpd *s)
{
	if (s == NULL) return;

	ffrq_free(s->kcq.cq);
	fftimer_close(s->timer, s->kq);
	ffsock_close(s->lsock);
	ffkq_close(s->kq);
	ffmem_free(s->kevents);
	ffmem_free(s->connections);
	ffmem_free(s);
}

static int sv_accept1(alphahttpd *s)
{
	if (s->conn_num == s->connections_n) {
		sv_warnlog(s, "reached max worker connections limit %u", s->connections_n);
		sv_timer(s, &s->tmr_fdlimit, -(int)s->conf.server.fdlimit_timeout_sec*1000, (fftimerqueue_func)sv_accept, s);
		return -1;
	}

	ffsock csock;
	ffsockaddr peer;
	if (FFSOCK_NULL == (csock = ffsock_accept_async(s->lsock, &peer, FFSOCK_NONBLOCK, s->sock_family, NULL, &s->lsock_kev.rtask_accept))) {
		if (fferr_last() == FFSOCK_EINPROGRESS)
			return -1;

		if (fferr_fdlimit(fferr_last())) {
			sv_syserrlog(s, "ffsock_accept");
			sv_timer(s, &s->tmr_fdlimit, -(int)s->conf.server.fdlimit_timeout_sec*1000, (fftimerqueue_func)sv_accept, s);
			return -1;
		}

		sv_syserrlog(s, "ffsock_accept");
		return -1;
	}

	struct ahd_kev *kev = s->reusable_connections_lifo;
	if (kev != NULL) {
		s->reusable_connections_lifo = kev->next_kev;
		kev->next_kev = NULL;
	} else {
		kev = &s->connections[s->iconn++];
	}
	sv_dbglog(s, "using connection slot #%u [%u]"
		, (uint)(kev - s->connections), s->conn_num + 1);

	uint conn_id = ffint_fetch_add(s->conf.server.conn_id_counter, 1);

	s->conn_num++;

	if (s->conf.kcq_set != NULL)
		kev->kcall.q = &s->kcq;

	cl_start(kev, csock, &peer, conn_id, s, &s->si);
	return 0;
}

/** Accept a bunch of client connections */
static void sv_accept(alphahttpd *s)
{
	for (;;) {
		if (0 != sv_accept1(s))
			break;
	}
}

static int sv_worker(alphahttpd *s)
{
	sv_dbglog(s, "entering kq loop");
	ffkq_time t;
	ffkq_time_set(&t, -1);
	if (s->conf.server.polling_mode)
		ffkq_time_set(&t, 0);

	while (!FFINT_READONCE(s->worker_stop)) {
		int r = ffkq_wait(s->kq, s->kevents, s->conf.server.events_num, t);

		for (int i = 0;  i < r;  i++) {
			ffkq_event *ev = &s->kevents[i];
			void *d = ffkq_event_data(ev);
			struct ahd_kev *c = (void*)((ffsize)d & ~1);

			if (((ffsize)d & 1) != c->side)
				continue;

			int flags = ffkq_event_flags(ev);
			// ffkq_task_event_assign();

#ifdef FF_WIN
			flags = FFKQ_READ;
			if (ev->lpOverlapped == &c->wtask.overlapped)
				flags = FFKQ_WRITE;
#endif

			sv_extralog(s, "%p #%L f:%xu r:%d w:%d"
				, c, c - s->connections, flags, c->rtask.active, c->wtask.active);

			if ((flags & FFKQ_READ) && c->rtask.active)
				c->rhandler(c->obj);
			if ((flags & FFKQ_WRITE) && c->wtask.active)
				c->whandler(c->obj);
		}

		if (r < 0 && fferr_last() != EINTR) {
			sv_sysfatallog(s, "ffkq_wait");
			return -1;
		}

		if (r > 1) {
			sv_extralog(s, "processed %u events", r);
		}

		if (s->conf.kcq_set != NULL)
			ffkcallq_process_cq(s->kcq.cq);
	}
	sv_dbglog(s, "leaving kq loop");
	return 0;
}

void sv_conn_fin(alphahttpd *s, struct ahd_kev *kev)
{
	kev->rhandler = NULL;
	kev->whandler = NULL;
	kev->side = !kev->side;
	kev->obj = NULL;

	kev->next_kev = s->reusable_connections_lifo;
	s->reusable_connections_lifo = kev;

	FF_ASSERT(s->conn_num != 0);
	s->conn_num--;
	sv_dbglog(s, "free connection slot #%u [%u]"
		, (uint)(kev - s->connections), s->conn_num);
}

static int sv_kq_attach(alphahttpd *s, ffsock sk, struct ahd_kev *kev, void *obj)
{
	kev->obj = obj;
	if (0 != ffkq_attach_socket(s->kq, sk, (void*)((ffsize)kev | kev->side), FFKQ_READWRITE)) {
		sv_syserrlog(s, "ffkq_attach_socket");
		return -1;
	}
	return 0;
}

/** Get thread ID */
// static ffuint64 sv_tid(alphahttpd *s);
// {
// 	return s->thd_id;
// }

/** Get cached calendar UTC date */
fftime sv_date(alphahttpd *s, ffstr *dts)
{
	fftime t = s->date_now;
	if (dts != NULL) {
		if (s->date_buf[0] == '\0') {
			ffdatetime dt;
			fftime_split1(&dt, &t);
			fftime_tostr1(&dt, s->date_buf, sizeof(s->date_buf), FFTIME_DATE_YMD | FFTIME_HMS_MSEC);
			s->date_buf[10] = 'T';
		}
		ffstr_set(dts, s->date_buf, sizeof(s->date_buf)-1);
	}

	return t;
}

static void sv_ontimer(alphahttpd *s)
{
	fftime_now(&s->date_now);
	s->date_now.sec += FFTIME_1970_SECONDS;

	fftime t = fftime_monotonic();
	s->timer_now_ms = t.sec*1000 + t.nsec/1000000;
	s->date_buf[0] = '\0';

	fftimerqueue_process(&s->timer_q, s->timer_now_ms);
	fftimer_consume(s->timer);
}

static int sv_timer_start(alphahttpd *s)
{
	if (FFTIMER_NULL == (s->timer = fftimer_create(0))) {
		sv_sysfatallog(s, "fftimer_create");
		return -1;
	}
	s->timer_kev.rhandler = (ahd_kev_func)sv_ontimer;
	s->timer_kev.obj = s;
	s->timer_kev.rtask.active = 1;
	if (0 != fftimer_start(s->timer, s->kq, &s->timer_kev, s->conf.server.timer_interval_msec)) {
		sv_sysfatallog(s, "fftimer_start");
		return -1;
	}
	fftimerqueue_init(&s->timer_q);
	sv_ontimer(s);
	return 0;
}

/** Add/restart/remove periodic/one-shot timer */
static void sv_timer(alphahttpd *s, ahd_timer *tmr, int interval_msec, fftimerqueue_func func, void *param)
{
	if (interval_msec == 0) {
		if (fftimerqueue_remove(&s->timer_q, tmr))
			sv_dbglog(s, "timer remove: %p", tmr);
		return;
	}

	fftimerqueue_add(&s->timer_q, tmr, s->timer_now_ms, interval_msec, func, param);
	sv_dbglog(s, "timer add: %p %d", tmr, interval_msec);
}

static void kcq_onsignal(alphahttpd *s)
{
	ffkcallq_process_cq(s->kcq.cq);
}
