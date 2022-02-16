/** alphahttpd: process command-line arguments
2022, Simon Zolin */

#include <util/cmdarg-scheme.h>
#include <util/ipaddr.h>
#include <FFOS/sysconf.h>

#define R_DONE  100
#define R_BADVAL  101

static int ip_port_split(ffstr s, void *ip6, ffushort *port)
{
	int r = 0;
	if (s.ptr[0] == '[') {
		r = ffip6_parse(ip6, s.ptr+1, s.len-1);
		if (r <= 0 || s.ptr[r+1] != ']')
			return -1;
		r += 2;
	} else {
		char ip4[4];
		r = ffip4_parse((void*)ip4, s.ptr, s.len);
		if (r > 0)
			ffip6_v4mapped_set(ip6, (void*)ip4);
		else
			r = 0;
	}

	if (r > 0) {
		if (s.ptr[r] != ':')
			return -1;
		ffstr_shift(&s, r+1);
	}

	if (!ffstr_toint(&s, port, FFS_INT16))
		return -1;
	return 0;
}

static int cmd_listen(void *cs, struct ahd_conf *conf, ffstr *val)
{
	if (0 != ip_port_split(*val, conf->bind_ip, &conf->listen_port))
		return R_BADVAL;
	return 0;
}

static int cmd_debug(void *cs, struct ahd_conf *conf)
{
	conf->log_level = LOG_DBG;
	return 0;
}

static int cmd_cpumask(void *cs, struct ahd_conf *conf, ffstr *val)
{
	if (!ffstr_toint(val, &conf->cpumask, FFS_INT32 | FFS_INTHEX))
		return R_BADVAL;
	return 0;
}

static int cmd_help()
{
	static const char help[] =
"Options:\n"
"-l, --listen ADDR   Listening IP and TCP port (def: 80)\n"
"                      e.g. 8080 or 127.0.0.1:8080 or [::1]:8080\n"
"-w, --www DIR       Web directory (def: www)\n"
"-t, --threads N     Worker threads (def: CPU#)\n"
"-c, --cpumask N     CPU affinity bitmask, hex value (e.g. 15 for CPUs 0,2,4)\n"
"-T, --kcall-threads N\n"
"                    kcall worker threads (def: CPU#)\n"
"-p, --polling       Active polling mode\n"
"-D, --debug         Debug log level\n"
"-h, --help          Show help\n"
;
	ffstdout_write(help, FFS_LEN(help));
	return R_DONE;
}

static const ffcmdarg_arg ahd_cmd_args[] = {
	{ 'l', "listen",	FFCMDARG_TSTR | FFCMDARG_FNOTEMPTY, (ffsize)cmd_listen },
	{ 'w', "www",	FFCMDARG_TSTR | FFCMDARG_FNOTEMPTY, FF_OFF(struct ahd_conf, www) },
	{ 't', "threads",	FFCMDARG_TINT32, FF_OFF(struct ahd_conf, workers_n) },
	{ 'T', "kcall-threads",	FFCMDARG_TINT32, FF_OFF(struct ahd_conf, kcall_workers) },
	{ 'c', "cpumask",	FFCMDARG_TSTR, (ffsize)cmd_cpumask },
	{ 'p', "polling",	FFCMDARG_TSWITCH, FF_OFF(struct ahd_conf, polling_mode) },
	{ 'D', "debug",	FFCMDARG_TSWITCH, (ffsize)cmd_debug },
	{ 'h', "help",	FFCMDARG_TSWITCH, (ffsize)cmd_help },
	{}
};

void conf_destroy(struct ahd_conf *conf)
{
	ffstr_free(&conf->www);
}

void conf_init(struct ahd_conf *conf)
{
	conf->listen_port = 80;
	conf->max_connections = 10000;
	conf->fd_limit = conf->max_connections * 2;
	conf->read_buf_size = 4096;
	conf->write_buf_size = 4096;
	conf->file_buf_size = 16*1024;
	conf->events_num = 1024;
	conf->tcp_nodelay = 1;
	conf->timer_interval_msec = 250;
	conf->max_keep_alive_reqs = 100;
	conf->log_level = LOG_INFO;

	conf->read_timeout_sec = 65;
	conf->write_timeout_sec = 65;
	conf->fdlimit_timeout_sec = 10;

	ffstr_dupz(&conf->www, "www");
	ffstr_setz(&conf->index_filename, "index.html");
}

int cmd_fin(struct ahd_conf *conf)
{
	if (!(ahd_conf->read_buf_size > 16 && ahd_conf->write_buf_size > 16)) {
		ffstderr_fmt("bad buffer sizes\n");
		return -1;
	}

	if (conf->workers_n == 0) {
		ffsysconf sc;
		ffsysconf_init(&sc);
		conf->workers_n = ffsysconf_get(&sc, FFSYSCONF_NPROCESSORS_ONLN);
		if (conf->cpumask == 0)
			conf->cpumask = (uint)-1;
	}
	if (conf->kcall_workers == 0)
		conf->kcall_workers = conf->workers_n;
	return 0;
}

int cmd_read(struct ahd_conf *conf, int argc, const char **argv)
{
	ffstr errmsg = {};
	int r = ffcmdarg_parse_object(ahd_cmd_args, conf, argv, argc, 0, &errmsg);
	if (r < 0) {
		if (r == -R_DONE)
			return -1;
		else if (r == -R_BADVAL)
			ffstderr_fmt("command line: bad value\n");
		else
			ffstderr_fmt("command line: %S\n", &errmsg);
		return -1;
	}
	if (0 != cmd_fin(conf))
		return -1;
	return 0;
}
