/** alphahttpd: server
2022, Simon Zolin */

#include <util/ipaddr.h>
#include <FFOS/queue.h>
#include <FFOS/socket.h>
#include <ffbase/atomic.h>

struct server {
	struct ahd_kev kev;
	ffsock lsock;
	ffkq kq;
	ffkq_event *kevents;
	struct ahd_kev *connections;
	struct ahd_kev *reusable_connections_lifo;
	uint iconn, conn_num;
	uint worker_stop;
	uint req_id;
	uint log_level;
};

static void sv_accept(struct server *s);

#define sv_sysfatallog(s, ...) \
	ahd_log(LOG_SYSFATAL, NULL, __VA_ARGS__)

#define sv_syserrlog(s, ...) \
	ahd_log(LOG_SYSERR, NULL, __VA_ARGS__)

#define sv_errlog(s, ...) \
	ahd_log(LOG_ERR, NULL, __VA_ARGS__)

#define sv_verblog(s, ...) \
do { \
	if (s->log_level >= LOG_VERB) \
		ahd_log(LOG_VERB, NULL, __VA_ARGS__); \
} while (0)

#define sv_dbglog(s, ...) \
do { \
	if (s->log_level >= LOG_DBG) \
		ahd_log(LOG_DBG, NULL, __VA_ARGS__); \
} while (0)

static int sv_prepare(struct server *s)
{
	s->kq = FFKQ_NULL;
	s->lsock = FFSOCK_NULL;
	s->log_level = ahd_conf->log_level;
	s->req_id = 1;

	if (!(ahd_conf->read_buf_size > 16 && ahd_conf->write_buf_size > 16)) {
		sv_errlog(s, "bad buffer sizes");
		return -1;
	}

	ffsock_init(FFSOCK_INIT_SIGPIPE);

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

		if (0 != ffsock_setopt(s->lsock, IPPROTO_IPV6, IPV6_V6ONLY, 0)) {
			sv_sysfatallog(s, "ffsock_setopt(IPV6_V6ONLY)");
			return -1;
		}
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

	sv_verblog(s, "listening on %u", ahd_conf->listen_port);
	return 0;
}

static void sv_destroy(struct server *s)
{
	ffsock_close(s->lsock);
	ffkq_close(s->kq);
	ffmem_free(s->kevents);
	ffmem_free(s->connections);
}

uint sv_req_id_next(struct server *s)
{
	return ffint_fetch_add(&s->req_id, 1);
}

static int sv_accept1(struct server *s)
{
	if (s->conn_num == ahd_conf->max_connections) {
		sv_errlog(s, "reached max parallel connections limit");
		return -1;
	}

	ffsock csock;
	ffsockaddr peer;
	if (FFSOCK_NULL == (csock = ffsock_accept(s->lsock, &peer, FFSOCK_NONBLOCK))) {
		if (fferr_again(fferr_last()))
			return -1;

		if (fferr_fdlimit(fferr_last())) {
			sv_syserrlog(s, "ffsock_accept");
			return -1;
		}

		sv_syserrlog(s, "ffsock_accept");
		return -1;
	}

	struct ahd_kev *kev = s->reusable_connections_lifo;
	if (kev != NULL) {
		s->reusable_connections_lifo = kev->next_kev;
		kev->next_kev = NULL;
		sv_dbglog(s, "reusing connection slot #%u", (uint)(kev - s->connections));
	} else {
		kev = &s->connections[s->iconn++];
	}

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

fftime sv_date(struct server *s, ffstr *dts)
{
	fftime t;
	fftime_now(&t);
	t.sec += FFTIME_1970_SECONDS;

	if (dts != NULL) {
		static char buf[FFS_LEN("0000-00-00T00:00:00.000")+1];
		ffdatetime dt;
		fftime_split1(&dt, &t);
		ffsize r = fftime_tostr1(&dt, buf, sizeof(buf), FFTIME_DATE_YMD | FFTIME_HMS_MSEC);
		buf[10] = 'T';
		ffstr_set(dts, buf, r);
	}

	return t;
}
