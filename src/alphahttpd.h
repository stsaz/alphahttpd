/** alphahttpd: shared types and functions
2022, Simon Zolin */

#pragma once
#include <FFOS/error.h>
#include <FFOS/kcall.h>
#include <FFOS/socket.h>
#include <FFOS/string.h>
#include <FFOS/thread.h>
#include <FFOS/time.h>
#include <FFOS/timerqueue.h>
#include <ffbase/vector.h>

#define AHD_VER "0.5"

typedef unsigned int uint;
typedef unsigned short ushort;

struct ahd_boss {
	ffvec workers; // struct server*[]
	uint conn_id;

	ffringqueue *kcq_sq;
	ffsem kcq_sem;
	ffvec kcq_workers; // ffthread[]
	uint kcq_stop;
};

typedef fftimerqueue_node ahd_timer;

struct ahd_conf {
	ffstr root_dir;
	char bind_ip[16];
	ffushort listen_port;
	ffbyte tcp_nodelay;
	uint log_level;
	uint max_connections;
	uint fd_limit;
	uint events_num;
	uint timer_interval_msec;
	uint workers_n;
	uint cpumask;
	uint kcall_workers;
	ffbyte polling_mode;

	uint read_buf_size, write_buf_size, file_buf_size;
	uint read_timeout_sec, write_timeout_sec, fdlimit_timeout_sec;
	uint max_keep_alive_reqs;
	ffstr index_filename;
	ffstr www;
};
extern struct ahd_conf *ahd_conf;

/** Convert relative file name to absolute file name using application directory */
char* conf_abs_filename(const char *rel_fn);

struct server;
struct server* sv_new(struct ahd_boss *boss);
void sv_free(struct server *s);
void sv_stop(struct server *s);
void sv_cpu_affinity(struct server *s, uint mask);
int sv_start(struct server *s);
int sv_run(struct server *s);

/** Get cached calendar UTC date */
fftime sv_date(struct server *s, ffstr *dts);

/** Get thread ID */
ffuint64 sv_tid(struct server *s);

/** Add/restart/remove periodic/one-shot timer */
void sv_timer(struct server *s, ahd_timer *tmr, int interval_msec, fftimerqueue_func func, void *param);
#define sv_timer_stop(s, tmr) \
	sv_timer(s, tmr, 0, NULL, NULL)

struct ahd_kev;
void sv_conn_fin(struct server *s, struct ahd_kev *kev);
int sv_kq_attach(struct server *s, ffsock sk, struct ahd_kev *kev, void *obj);
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


enum LOG {
	LOG_SYSFATAL,
	LOG_SYSERR,
	LOG_ERR,
	LOG_SYSWARN,
	LOG_WARN,
	LOG_INFO,
	LOG_VERB,
	LOG_DBG,
};

/** Add line to log
level: enum LOG */
void ahd_log(struct server *s, uint level, const char *id, const char *fmt, ...);


/** Initialize HTTP modules */
void http_mods_init();

/** Start processing the client */
void cl_start(struct ahd_kev *kev, ffsock csock, const ffsockaddr *peer, struct server *s, uint conn_id);
