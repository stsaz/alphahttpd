/** alphahttpd: startup
2022, Simon Zolin */

#include <alphahttpd.h>
#ifdef FF_WIN
typedef unsigned int uint;
#endif
#include <cmd.h>
#include <FFOS/signal.h>
#include <FFOS/thread.h>
#include <FFOS/ffos-extern.h>
#ifdef FF_UNIX
#include <sys/resource.h>
#endif

#define AHD_VER "0.5"

struct worker {
	alphahttpd *srv;
	ffthread thd;
};

struct ahd_boss {
	ffvec workers; // struct worker[]
	uint conn_id;

	ffringqueue *kcq_sq;
	ffsem kcq_sem;
	ffvec kcq_workers; // ffthread[]
	uint kcq_stop;

	uint stdout_color;
};

static struct ahd_conf *ahd_conf;
static struct ahd_boss *boss;

#define sysfatallog(...) \
	ahd_log(NULL, ALPHAHTTPD_LOG_SYSFATAL, NULL, __VA_ARGS__)
#define syserrlog(...) \
	ahd_log(NULL, ALPHAHTTPD_LOG_SYSERR, NULL, __VA_ARGS__)
#define dbglog(...) \
do { \
	if (ahd_conf->aconf.log_level >= ALPHAHTTPD_LOG_DEBUG) \
		ahd_log(NULL, ALPHAHTTPD_LOG_DEBUG, NULL, __VA_ARGS__); \
} while (0)

#include <log.h>
#include <kcq.h>

static int FFTHREAD_PROCCALL wrk_thread(struct worker *w)
{
	dbglog("worker: started");
	return alphahttpd_run(w->srv);
}

static int wrk_start(struct worker *w)
{
	if (FFTHREAD_NULL == (w->thd = ffthread_create((ffthread_proc)wrk_thread, w, 0))) {
		syserrlog("thread create");
		return -1;
	}
	return 0;
}

#ifdef FF_LINUX
typedef cpu_set_t _cpuset;
#elif defined FF_BSD
typedef cpuset_t _cpuset;
#endif

static void wrk_cpu_affinity(struct worker *w, uint icpu)
{
#ifdef FF_UNIX
	_cpuset cpuset;
	CPU_ZERO(&cpuset);
	CPU_SET(icpu, &cpuset);
	ffthread t = (w->thd != FFTHREAD_NULL) ? w->thd : pthread_self();
	if (0 != pthread_setaffinity_np(t, sizeof(cpuset), &cpuset)) {
		syserrlog("set CPU affinity");
		return;
	}
	dbglog("worker %p: CPU affinity: %u", w, icpu);
#endif
}

/** Send stop signal to all workers */
static void boss_stop()
{
	struct worker *w;
	FFSLICE_WALK(&boss->workers, w) {
		alphahttpd_stop(w->srv);
	}
}

static void wrk_destroy(struct worker *w)
{
	alphahttpd_stop(w->srv);
	if (w->thd != FFTHREAD_NULL) {
		ffthread_join(w->thd, -1, NULL);
	}
	alphahttpd_free(w->srv);
}

static void boss_destroy()
{
	if (boss == NULL)
		return;

	struct worker *w;
	FFSLICE_WALK(&boss->workers, w) {
		wrk_destroy(w);
	}
	ffvec_free(&boss->workers);

	kcq_destroy();
}

static void cpu_affinity()
{
	if (ahd_conf->cpumask == 0)
		return;

	uint mask = ahd_conf->cpumask;
	struct worker *w;
	FFSLICE_WALK(&boss->workers, w) {
		uint n = ffbit_rfind32(mask);
		if (n == 0)
			break;
		n--;
		ffbit_reset32(&mask, n);
		wrk_cpu_affinity(w, n);
	}
}

static void onsig(struct ffsig_info *i)
{
	boss_stop();
}

extern const struct alphahttpd_filter* ah_filters[];

/** Initialize HTTP modules */
static void http_mods_init(struct alphahttpd_conf *aconf)
{
	ffvec v = {};
	char *fn = conf_abs_filename(ahd_conf, "content-types.conf");
	if (0 == fffile_readwhole(fn, &v, 64*1024)) {
		alphahttpd_filter_file_init(aconf, *(ffstr*)&v);
		ffvec_null(&v);
	}
	ffvec_free(&v);
	ffmem_free(fn);
}

static void aconf_setup(struct ahd_conf *conf)
{
	struct alphahttpd_conf *ac = &conf->aconf;
	ac->log = ahd_log;
	ac->logv = ahd_logv;
	ac->kcq_set = kcq_set;
	ac->filters = (const struct alphahttpd_filter**)ah_filters;
	ac->server.conn_id_counter = &boss->conn_id;
	http_mods_init(ac);
}

/** Check if fd is a terminal */
static int std_console(fffd fd)
{
#ifdef FF_WIN
	DWORD r;
	return GetConsoleMode(fd, &r);

#else
	fffileinfo fi;
	return (0 == fffile_info(fd, &fi)
		&& FFFILE_UNIX_CHAR == (fffileinfo_attr(&fi) & FFFILE_UNIX_TYPEMASK));
#endif
}

int main(int argc, char **argv)
{
	static const char appname[] = "αhttpd v" AHD_VER "\n";
	ffstdout_write(appname, FFS_LEN(appname));

	ahd_conf = ffmem_new(struct ahd_conf);
	conf_init(ahd_conf);
	if (0 != cmd_read(ahd_conf, argc, (const char**)argv))
		goto end;

#ifdef FF_UNIX
	if (ahd_conf->fd_limit != 0) {
		struct rlimit rl;
		rl.rlim_cur = ahd_conf->fd_limit;
		rl.rlim_max = ahd_conf->fd_limit;
		setrlimit(RLIMIT_NOFILE, &rl);
	}
#endif

	boss = ffmem_new(struct ahd_boss);
	boss->conn_id = 1;

#ifdef FF_WIN
	boss->stdout_color = (0 == ffstd_attr(ffstdout, FFSTD_VTERM, FFSTD_VTERM));
	(void)std_console;
#else
	boss->stdout_color = std_console(ffstdout);
#endif

	if (0 != ffsock_init(FFSOCK_INIT_SIGPIPE | FFSOCK_INIT_WSA | FFSOCK_INIT_WSAFUNCS))
		goto end;
	if (0 != kcq_init())
		goto end;

	aconf_setup(ahd_conf);

	ffvec_allocT(&boss->workers, ahd_conf->workers_n, struct worker);
	boss->workers.len = ahd_conf->workers_n;
	struct worker *w;
	FFSLICE_WALK(&boss->workers, w) {
		w->srv = alphahttpd_new();
		if (w == boss->workers.ptr)
			ahd_conf->aconf.opaque = w->srv;
		alphahttpd_conf(w->srv, &ahd_conf->aconf);
	}

	if (0 != kcq_start())
		goto end;

	FFSLICE_WALK(&boss->workers, w) {
		if (w != boss->workers.ptr) {
			if (0 != wrk_start(w))
				goto end;
		}
	}

	cpu_affinity();

	static const uint sigs[] = { FFSIG_INT };
	ffsig_subscribe(onsig, sigs, FF_COUNT(sigs));

	w = boss->workers.ptr;
	if (0 != alphahttpd_run(w->srv))
		goto end;

end:
	boss_destroy();
	ffmem_free(boss);
	conf_destroy(ahd_conf);
	ffmem_free(ahd_conf);
	return 0;
}
