/** alphahttpd: server interface
2023, Simon Zolin */

#pragma once
#include <FFOS/kcall.h>
#include <FFOS/socket.h>
#include <ffbase/map.h>

typedef struct alphahttpd alphahttpd;
typedef struct alphahttpd_client alphahttpd_client;
struct alphahttpd_filter;

enum ALPHAHTTPD_LOG {
	ALPHAHTTPD_LOG_SYSFATAL,
	ALPHAHTTPD_LOG_SYSERR,
	ALPHAHTTPD_LOG_ERR,
	ALPHAHTTPD_LOG_SYSWARN,
	ALPHAHTTPD_LOG_WARN,
	ALPHAHTTPD_LOG_INFO,
	ALPHAHTTPD_LOG_VERBOSE,
	ALPHAHTTPD_LOG_DEBUG,
	ALPHAHTTPD_LOG_EXTRA,
};

struct alphahttpd_address {
	ffbyte ip[16];
	ffuint port;
};

struct alphahttpd_conf {
	void *opaque;
	ffuint log_level;
	void (*log)(void *opaque, ffuint level, const char *id, const char *format, ...);
	void (*logv)(void *opaque, ffuint level, const char *id, const char *format, va_list va);

	/** Set kcq->sq and kcq->sem.
	If NULL, KCQ mechanism will be disabled. */
	void (*kcq_set)(void *opaque, struct ffkcallqueue *kcq);

	struct {
		const struct alphahttpd_address *listen_addresses;
		ffuint max_connections;
		ffuint events_num;
		ffuint fdlimit_timeout_sec;
		ffuint timer_interval_msec;
		ffuint _conn_id_counter_default;
		ffuint *conn_id_counter;
		ffbyte polling_mode;
	} server;

	const struct alphahttpd_filter **filters;

	ffuint max_keep_alive_reqs;

	struct {
		ffuint buf_size;
		ffuint timeout_sec;
	} receive;

	struct {
		ffstr www;
		ffstr index_filename;
		ffuint file_buf_size;

		ffmap content_types_map;
		char *content_types_data;
	} fs;

	struct {
		ffuint buf_size;
		ffstr server_name;
	} response;

	struct {
		ffbyte tcp_nodelay;
		ffuint timeout_sec;
	} send;

	struct {
		ffmap map;
	} virtspace;
};

FF_EXTERN alphahttpd* alphahttpd_new();
FF_EXTERN void alphahttpd_free(alphahttpd *srv);

/** Set server configuration
srv==NULL: initialize `conf` with default settings */
FF_EXTERN int alphahttpd_conf(alphahttpd *srv, struct alphahttpd_conf *conf);

/** Run server's event loop */
FF_EXTERN int alphahttpd_run(alphahttpd *srv);

/** Send stop-signal to the worker thread */
FF_EXTERN void alphahttpd_stop(alphahttpd *srv);

/** file: initialize content-type map
content_types: heap buffer (e.g. "text/html	htm html\r\n"); user must not use it afterwards */
FF_EXTERN void alphahttpd_filter_file_init(struct alphahttpd_conf *conf, ffstr content_types);

FF_EXTERN void alphahttpd_filter_file_uninit(struct alphahttpd_conf *conf);

struct alphahttpd_virtdoc {
	const char *path, *method;

	/** Called by virtspace filter to handle the requested document.
	The handler must set resp.content_length, response status, 'resp_done' flag.
	If resp.content_length is not set, empty '200 OK' response is returned. */
	void (*handler)(alphahttpd_client *c);
};

/** Prepare the table of virtual documents.
docs: static array (must be valid while the module is in use) */
FF_EXTERN int alphahttpd_filter_virtspace_init(struct alphahttpd_conf *conf, const struct alphahttpd_virtdoc *docs);

FF_EXTERN void alphahttpd_filter_virtspace_uninit(struct alphahttpd_conf *conf);
