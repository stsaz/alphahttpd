/** alphahttpd: server
2022, Simon Zolin */

#include <alphahttpd.h>
#include <util/ipaddr.h>
#include <FFOS/queue.h>
#include <FFOS/socket.h>
#include <FFOS/timer.h>
#include <FFOS/perf.h>
#include <FFOS/thread.h>
#include <ffbase/atomic.h>

struct server {
	struct ahd_kev kev;
	struct ahd_boss *boss;
	ffthread thd;
	ffuint64 thd_id;
	ffsock lsock;
	ffkq kq;
	ffkq_event *kevents;
	struct ahd_kev *connections;
	struct ahd_kev *reusable_connections_lifo;
	uint iconn, conn_num;
	ahd_timer tmr_fdlimit;
	struct ahd_kev post_kev;
	ffkq_postevent kqpost;
	uint worker_stop;
	uint log_level;

	fftimer timer;
	fftimerqueue timer_q;
	struct ahd_kev timer_kev;
	uint timer_now_ms;
	fftime date_now;
	char date_buf[FFS_LEN("0000-00-00T00:00:00.000")+1];
};

static void sv_accept(struct server *s);
static int sv_timer_start(struct server *s);
static int sv_worker(struct server *s);

#define sv_sysfatallog(s, ...) \
	ahd_log(s, LOG_SYSFATAL, NULL, __VA_ARGS__)

#define sv_syserrlog(s, ...) \
	ahd_log(s, LOG_SYSERR, NULL, __VA_ARGS__)

#define sv_errlog(s, ...) \
	ahd_log(s, LOG_ERR, NULL, __VA_ARGS__)

#define sv_verblog(s, ...) \
do { \
	if (s->log_level >= LOG_VERB) \
		ahd_log(s, LOG_VERB, NULL, __VA_ARGS__); \
} while (0)

#define sv_dbglog(s, ...) \
do { \
	if (s->log_level >= LOG_DBG) \
		ahd_log(s, LOG_DBG, NULL, __VA_ARGS__); \
} while (0)

struct server* sv_new(struct ahd_boss *boss)
{
	struct server *s = ffmem_new(struct server);
	s->thd = FFTHREAD_NULL;
	s->kq = FFKQ_NULL;
	s->lsock = FFSOCK_NULL;
	s->log_level = ahd_conf->log_level;
	s->boss = boss;
	return s;
}

int sv_start(struct server *s)
{
	if (FFTHREAD_NULL == (s->thd = ffthread_create((ffthread_proc)(void*)sv_run, s, 0))) {
		sv_sysfatallog(s, "thread create");
		return -1;
	}
	return 0;
}

void sv_stop(struct server *s)
{
	FFINT_WRITEONCE(s->worker_stop, 1);
	ffkq_post(s->kqpost, &s->post_kev);
	if (s->thd != FFTHREAD_NULL) {
		ffthread_join(s->thd, -1, NULL);
	}
}

void sv_cpu_affinity(struct server *s, uint icpu)
{
	cpu_set_t cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(icpu, &cpuset);
	ffthread t = (s->thd != FFTHREAD_NULL) ? s->thd : pthread_self();
	if (0 != pthread_setaffinity_np(t, sizeof(cpuset), &cpuset)) {
		sv_syserrlog(s, "set CPU affinity");
		return;
	}
	sv_dbglog(s, "CPU affinity: %u", icpu);
}

int FFTHREAD_PROCCALL sv_run(struct server *s)
{
	s->thd_id = ffthread_curid();

	s->connections = (void*)ffmem_alloc(ahd_conf->max_connections * sizeof(struct ahd_kev));
	s->kevents = (void*)ffmem_alloc(ahd_conf->events_num * sizeof(ffkq_event));
	if (s->connections == NULL || s->kevents == NULL) {
		sv_sysfatallog(s, "no memory");
		return -1;
	}

	if (FFKQ_NULL == (s->kq = ffkq_create())) {
		sv_sysfatallog(s, "ffkq_create");
		return -1;
	}
	if (FFKQ_NULL == (s->kqpost = ffkq_post_attach(s->kq, &s->post_kev))) {
		sv_sysfatallog(s, "ffkq_post_attach");
		return -1;
	}

	const void *ip4 = ffip6_tov4((void*)ahd_conf->bind_ip);
	int fam = (ip4 != NULL) ? AF_INET : AF_INET6;
	if (FFSOCK_NULL == (s->lsock = ffsock_create_tcp(fam, FFSOCK_NONBLOCK))) {
		sv_sysfatallog(s, "ffsock_create_tcp");
		return -1;
	}

	ffsockaddr addr = {};
	if (ip4 != NULL) {
		ffsockaddr_set_ipv4(&addr, ip4, ahd_conf->listen_port);
	} else {
		ffsockaddr_set_ipv6(&addr, ahd_conf->bind_ip, ahd_conf->listen_port);

		// Allow clients to connect via IPv4
		if (ffip6_isany((void*)ahd_conf->bind_ip)
			&& 0 != ffsock_setopt(s->lsock, IPPROTO_IPV6, IPV6_V6ONLY, 0)) {
			sv_sysfatallog(s, "ffsock_setopt(IPV6_V6ONLY)");
			return -1;
		}
	}

	// Allow several listening sockets to bind to the same address/port.
	// OS automatically distributes the load among the sockets.
	if (0 != ffsock_setopt(s->lsock, SOL_SOCKET, SO_REUSEPORT, 1)) {
		sv_sysfatallog(s, "ffsock_setopt(SO_REUSEPORT)");
		return -1;
	}

	if (0 != ffsock_bind(s->lsock, &addr)) {
		sv_sysfatallog(s, "socket bind");
		return -1;
	}

	if (0 != ffsock_listen(s->lsock, SOMAXCONN)) {
		sv_sysfatallog(s, "socket listen");
		return -1;
	}

	s->kev.rhandler = (ahd_kev_func)sv_accept;
	s->kev.obj = s;
	if (0 != ffkq_attach_socket(s->kq, s->lsock, &s->kev, FFKQ_READ)) {
		sv_sysfatallog(s, "ffkq_attach_socket");
		return -1;
	}

	if (0 != sv_timer_start(s))
		return -1;

	sv_verblog(s, "listening on %u", ahd_conf->listen_port);
	sv_worker(s);
	return 0;
}

void sv_free(struct server *s)
{
	fftimer_close(s->timer, s->kq);
	ffsock_close(s->lsock);
	ffkq_close(s->kq);
	ffmem_free(s->kevents);
	ffmem_free(s->connections);
	ffmem_free(s);
}

uint sv_req_id_next(struct server *s)
{
	return ffint_fetch_add(&s->boss->req_id, 1);
}

static int sv_accept1(struct server *s)
{
	if (s->conn_num == ahd_conf->max_connections) {
		sv_errlog(s, "reached max parallel connections limit %u", ahd_conf->max_connections);
		sv_timer(s, &s->tmr_fdlimit, -(int)ahd_conf->fdlimit_timeout_sec*1000, (fftimerqueue_func)sv_accept, s);
		return -1;
	}

	ffsock csock;
	ffsockaddr peer;
	if (FFSOCK_NULL == (csock = ffsock_accept(s->lsock, &peer, FFSOCK_NONBLOCK))) {
		if (fferr_again(fferr_last()))
			return -1;

		if (fferr_fdlimit(fferr_last())) {
			sv_syserrlog(s, "ffsock_accept");
			sv_timer(s, &s->tmr_fdlimit, -(int)ahd_conf->fdlimit_timeout_sec*1000, (fftimerqueue_func)sv_accept, s);
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

	s->conn_num++;
	cl_start(kev, csock, &peer, s);
	return 0;
}

static void sv_accept(struct server *s)
{
	for (;;) {
		if (0 != sv_accept1(s))
			break;
	}
}

static int sv_worker(struct server *s)
{
	ffkq_time t;
	ffkq_time_set(&t, -1);
	while (!FFINT_READONCE(s->worker_stop)) {
		int r = ffkq_wait(s->kq, s->kevents, ahd_conf->events_num, t);

		for (int i = 0;  i < r;  i++) {
			ffkq_event *ev = &s->kevents[i];
			void *d = ffkq_event_data(ev);
			struct ahd_kev *c = (void*)((ffsize)d & ~1);

			if (((ffsize)d & 1) != c->side)
				continue;

			int flags = ffkq_event_flags(ev);
			if ((flags & FFKQ_READ) && c->rhandler != NULL)
				c->rhandler(c->obj);
			if ((flags & FFKQ_WRITE) && c->whandler != NULL)
				c->whandler(c->obj);
		}

		if (r < 0 && fferr_last() != EINTR) {
			sv_sysfatallog(s, "ffkq_wait");
			return -1;
		}
	}
	sv_dbglog(s, "leaving kq loop");
	return 0;
}

void sv_conn_fin(struct server *s, struct ahd_kev *kev)
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

int sv_kq_attach(struct server *s, ffsock sk, struct ahd_kev *kev, void *obj)
{
	kev->obj = obj;
	if (0 != ffkq_attach_socket(s->kq, sk, (void*)((ffsize)kev | kev->side), FFKQ_READWRITE)) {
		sv_syserrlog(s, "ffkq_attach_socket");
		return -1;
	}
	return 0;
}

ffuint64 sv_tid(struct server *s)
{
	return s->thd_id;
}

fftime sv_date(struct server *s, ffstr *dts)
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

static void sv_ontimer(struct server *s)
{
	fftime_now(&s->date_now);
	s->date_now.sec += FFTIME_1970_SECONDS;

	fftime t = fftime_monotonic();
	s->timer_now_ms = t.sec*1000 + t.nsec/1000000;
	s->date_buf[0] = '\0';

	fftimerqueue_process(&s->timer_q, s->timer_now_ms);
	fftimer_consume(s->timer);
}

static int sv_timer_start(struct server *s)
{
	if (FFTIMER_NULL == (s->timer = fftimer_create(0))) {
		sv_sysfatallog(s, "fftimer_create");
		return -1;
	}
	s->timer_kev.rhandler = (ahd_kev_func)(void*)sv_ontimer;
	s->timer_kev.obj = s;
	if (0 != fftimer_start(s->timer, s->kq, &s->timer_kev, ahd_conf->timer_interval_msec)) {
		sv_sysfatallog(s, "fftimer_start");
		return -1;
	}
	fftimerqueue_init(&s->timer_q);
	sv_ontimer(s);
	return 0;
}

void sv_timer(struct server *s, ahd_timer *tmr, int interval_msec, fftimerqueue_func func, void *param)
{
	if (interval_msec == 0) {
		if (fftimerqueue_remove(&s->timer_q, tmr))
			sv_dbglog(s, "timer remove: %p", tmr);
		return;
	}

	fftimerqueue_add(&s->timer_q, tmr, s->timer_now_ms, interval_msec, func, param);
	sv_dbglog(s, "timer add: %p %d", tmr, interval_msec);
}
