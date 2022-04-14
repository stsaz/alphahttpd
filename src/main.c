/** alphahttpd: startup
2022, Simon Zolin */

#include <alphahttpd.h>
#include <log.h>
#include <cmdline.h>
#include <FFOS/signal.h>
#include <FFOS/ffos-extern.h>
#include <sys/resource.h>

int _ffcpu_features;
struct ahd_conf *ahd_conf;

static struct ahd_boss *boss;

static int FFTHREAD_PROCCALL kcq_worker(void *param);
static int kcq_init()
{
	if (NULL == (boss->kcq_sq = ffrq_alloc(ahd_conf->max_connections))) {
		return -1;
	}
	if (!ahd_conf->polling_mode
		&& FFSEM_NULL == (boss->kcq_sem = ffsem_open(NULL, 0, 0))) {
		return -1;
	}
	if (NULL == ffvec_allocT(&boss->kcq_workers, ahd_conf->kcall_workers, ffthread)) {
		return -1;
	}
	boss->kcq_workers.len = ahd_conf->kcall_workers;
	ffthread *it;
	FFSLICE_WALK(&boss->kcq_workers, it) {
		if (FFTHREAD_NULL == (*it = ffthread_create(kcq_worker, boss, 0))) {
			return -1;
		}
	}
	return 0;
}

static void kcq_destroy()
{
	// sv_dbglog(s, "stopping kcall worker");
	FFINT_WRITEONCE(boss->kcq_stop, 1);
	ffcpu_fence_release();
	ffthread *it;
	if (boss->kcq_sem != FFSEM_NULL) {
		FFSLICE_WALK(&boss->kcq_workers, it) {
			ffsem_post(boss->kcq_sem);
		}
	}
	FFSLICE_WALK(&boss->kcq_workers, it) {
		if (*it != FFTHREAD_NULL)
			ffthread_join(*it, -1, NULL);
	}
	if (boss->kcq_sem != FFSEM_NULL)
		ffsem_close(boss->kcq_sem);
	ffrq_free(boss->kcq_sq);
	ffvec_free(&boss->kcq_workers);
}

static int FFTHREAD_PROCCALL kcq_worker(void *param)
{
	// sv_dbglog(s, "entering kcall loop");
	uint polling_mode = ahd_conf->polling_mode;
	while (!FFINT_READONCE(boss->kcq_stop)) {
		ffkcallq_process_sq(boss->kcq_sq);
		if (!polling_mode)
			ffsem_wait(boss->kcq_sem, -1);
	}
	// sv_dbglog(s, "leaving kcall loop");
	return 0;
}

static void boss_stop()
{
	struct server **it;
	FFSLICE_WALK(&boss->workers, it) {
		struct server *s = *it;
		sv_stop(s);
	}
}

static void boss_destroy()
{
	if (boss == NULL)
		return;

	struct server **it;
	FFSLICE_WALK(&boss->workers, it) {
		struct server *s = *it;
		sv_stop(s);
		sv_free(s);
	}
	ffvec_free(&boss->workers);

	kcq_destroy();
}

static void cpu_affinity()
{
	if (ahd_conf->cpumask == 0)
		return;

	uint mask = ahd_conf->cpumask;
	struct server **it;
	FFSLICE_WALK(&boss->workers, it) {
		struct server *s = *it;
		uint n = ffbit_rfind32(mask);
		if (n == 0)
			break;
		n--;
		ffbit_reset32(&mask, n);
		sv_cpu_affinity(s, n);
	}
}

static void onsig(struct ffsig_info *i)
{
	boss_stop();
}

int main(int argc, char **argv)
{
	static char appname[] = "αhttpd " AHD_VER "\n";
	ffstdout_write(appname, FFS_LEN(appname));

	ahd_conf = ffmem_new(struct ahd_conf);
	conf_init(ahd_conf);
	if (0 != cmd_read(ahd_conf, argc, (const char**)argv))
		goto end;

	if (ahd_conf->fd_limit != 0) {
		struct rlimit rl;
		rl.rlim_cur = ahd_conf->fd_limit;
		rl.rlim_max = ahd_conf->fd_limit;
		setrlimit(RLIMIT_NOFILE, &rl);
	}

	boss = ffmem_new(struct ahd_boss);
	boss->conn_id = 1;

	ffsock_init(FFSOCK_INIT_SIGPIPE | FFSOCK_INIT_WSA | FFSOCK_INIT_WSAFUNCS);
	http_mods_init();
	if (0 != kcq_init())
		goto end;

	ffvec_allocT(&boss->workers, ahd_conf->workers_n, void*);
	boss->workers.len = ahd_conf->workers_n;
	struct server **it;
	FFSLICE_WALK(&boss->workers, it) {
		struct server *s = sv_new(boss);
		*it = s;
		if (it != boss->workers.ptr) {
			if (0 != sv_start(s)) {
				goto end;
			}
		}
	}

	cpu_affinity();

	static const uint sigs[] = { SIGINT };
	ffsig_subscribe(onsig, sigs, FF_COUNT(sigs));

	struct server *s = *(struct server**)boss->workers.ptr;
	if (0 != sv_run(s))
		goto end;

end:
	boss_destroy();
	ffmem_free(boss);
	conf_destroy(ahd_conf);
	ffmem_free(ahd_conf);
	return 0;
}
