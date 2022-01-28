/** alphahttpd: startup
2022, Simon Zolin */

#include <alphahttpd.h>
#include <log.h>
#include <cmdline.h>
#include <sys/resource.h>

int _ffcpu_features;
struct ahd_conf *ahd_conf;

int main(int argc, char **argv)
{
	static char appname[] = "αhttpd " AHD_VER "\n";
	ffstdout_write(appname, sizeof(appname)-1);

	struct server *s = NULL;
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

	s = sv_new();
	log_s = s;
	if (0 != sv_run(s))
		goto end;

end:
	if (s != NULL) {
		sv_free(s);
	}
	conf_destroy(ahd_conf);
	ffmem_free(ahd_conf);
	return 0;
}
