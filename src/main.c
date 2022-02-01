/** alphahttpd: startup
2022, Simon Zolin */

#include <alphahttpd.h>
#include <log.h>
#include <cmdline.h>
#include <sys/resource.h>
#include <FFOS/signal.h>
#include <FFOS/ffos-extern.h>

int _ffcpu_features;
struct ahd_conf *ahd_conf;

static struct ahd_boss *boss;

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
	boss_destroy();
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

	ffsock_init(FFSOCK_INIT_SIGPIPE);

	boss = ffmem_new(struct ahd_boss);
	boss->req_id = 1;

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
