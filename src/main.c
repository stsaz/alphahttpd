/** alphahttpd: startup
2022, Simon Zolin */

#include <alphahttpd.h>
#include <log.h>
#include <server.h>
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

	struct rlimit rl;
	rl.rlim_cur = ahd_conf->fd_limit;
	rl.rlim_max = ahd_conf->fd_limit;
	setrlimit(RLIMIT_NOFILE, &rl);

	s = ffmem_new(struct server);
	if (0 != sv_prepare(s))
		goto end;
	sv_worker(s);

end:
	if (s != NULL) {
		sv_destroy(s);
		ffmem_free(s);
	}
	conf_destroy(ahd_conf);
	ffmem_free(ahd_conf);
	return 0;
}
