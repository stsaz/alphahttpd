/** alphahttpd: [DESCRIPTION]
[YEAR], [AUTHOR] */

#include <http/client.h>

static int ah[NAME]_open(alphahttpd_client *c)
{
	return AHFILTER_FWD;
}

static void ah[NAME]_close(alphahttpd_client *c)
{
}

static int ah[NAME]_process(alphahttpd_client *c)
{
	return AHFILTER_DONE;
}

const struct alphahttpd_filter alphahttpd_filter_[NAME] = {
	ah[NAME]_open, ah[NAME]_close, ah[NAME]_process
};
