/** alphahttpd: shared types and functions
2022, Simon Zolin */

#pragma once
#include <FFOS/string.h>
#include <FFOS/error.h>
#include <FFOS/socket.h>
#include <FFOS/time.h>

#define AHD_VER "v0.1"

struct ahd_conf {
	char bind_ip[16];
	ffushort listen_port;
	ffbyte tcp_nodelay;
	uint log_level;
	uint max_connections;
	uint fd_limit;
	uint events_num;

	uint read_buf_size, write_buf_size, file_buf_size;
	uint max_keep_alive_reqs;
	ffstr index_filename;
	ffstr www;
};
extern struct ahd_conf *ahd_conf;

struct server;
fftime sv_date(struct server *s, ffstr *dts);
uint sv_req_id_next(struct server *s);
struct ahd_kev;
void sv_conn_fin(struct server *s, struct ahd_kev *kev);
int sv_kq_attach(struct server *s, ffsock sk, struct ahd_kev *kev, void *obj);
typedef void (*ahd_kev_func)(void *obj);
struct ahd_kev {
	ahd_kev_func rhandler, whandler;
	uint side;
	void *obj;
	struct ahd_kev *next_kev;
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

/** level: enum LOG */
void ahd_log(uint level, const char *id, const char *fmt, ...);


/** Start processing the client */
void cl_start(struct ahd_kev *kev, ffsock csock, const ffsockaddr *peer, struct server *s);
