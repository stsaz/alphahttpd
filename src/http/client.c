/** alphahttpd: client
2022, Simon Zolin */

/* Modules chain when serving a file:
receive <-> request -> index -> file/error <-> content-length <-> response -> send -> access-log
*/

#include <http/client.h>
#include <util/ipaddr.h>
#include <FFOS/socket.h>
#include <FFOS/file.h>
#include <ffbase/vector.h>

extern void sv_conn_fin(alphahttpd *srv, struct ahd_kev *kev);
static void cl_filters_process(alphahttpd_client *c);

static void cl_init(alphahttpd_client *c)
{
	c->resp.content_length = (ffuint64)-1;
}

/** Start processing the client */
void cl_start(struct ahd_kev *kev, ffsock csock, const ffsockaddr *peer, uint conn_id, alphahttpd *srv, struct ahd_server *si)
{
	alphahttpd_client *c = ffmem_alloc(sizeof(alphahttpd_client));
	if (c == NULL) {
		si->conf->log(si->conf->opaque, ALPHAHTTPD_LOG_ERR, NULL, "no memory");
		return;
	}
	ffmem_zero_obj(c);
	c->sk = csock;
	c->srv = srv;
	c->si = si;
	c->conf = si->conf;
	c->opaque = si->conf->opaque;
	c->log_level = si->conf->log_level;
	c->log = si->conf->log;

	c->kev = kev;
	c->kev->rhandler = (void*)cl_filters_process;
	c->kev->whandler = (void*)cl_filters_process;
	c->kev->kcall.handler = (void*)cl_filters_process;
	c->kev->kcall.param = c;

	c->id[0] = '*';
	int r = ffs_fromint(conn_id, c->id+1, sizeof(c->id)-1, 0);
	c->id[r+1] = '\0';

	uint port = 0;
	ffslice ip = ffsockaddr_ip_port(peer, &port);
	if (ip.len == 4)
		ffip6_v4mapped_set((ffip6*)c->peer_ip, (ffip4*)ip.ptr);
	else
		ffmem_copy(c->peer_ip, ip.ptr, ip.len);
	c->peer_port = port;

	if (c->log_level >= ALPHAHTTPD_LOG_VERBOSE) {
		char buf[FFIP6_STRLEN];
		r = ffip46_tostr((void*)c->peer_ip, buf, sizeof(buf));
		cl_verblog(c, "new client connection: %*s:%u"
			, (ffsize)r, buf, c->peer_port);
	}

#ifdef FF_WIN
	if (0 != cl_async(c))
		return;
#endif

	cl_init(c);
	cl_filters_process(c);
}

static void cl_mods_close(alphahttpd_client *c)
{
	for (uint i = 0;  c->conf->filters[i] != NULL;  i++) {
		if (c->mdata[i].opened) {
			cl_extralog(c, "filter %u: closing", i);
			c->conf->filters[i]->close(c);
		}
	}
}

void cl_destroy(alphahttpd_client *c)
{
	cl_verblog(c, "closing client connection");
	ffsock_close(c->sk);
	ffkcall_cancel(&c->kev->kcall);

	cl_mods_close(c);
	sv_conn_fin(c->srv, c->kev);
	ffmem_free(c);
}

static void cl_reset(alphahttpd_client *c)
{
	c->ka = 1;
	cl_mods_close(c);

	ffvec rb = c->req.buf; // preserve pipelined data

	ffmem_zero(&c->start_time_msec, sizeof(*c) - FF_OFF(alphahttpd_client, start_time_msec));
	ffkcall_cancel(&c->kev->kcall);
	cl_init(c);

	c->req.buf = rb;
}

static int cl_keepalive(alphahttpd_client *c)
{
	if (!c->resp_connection_keepalive)
		return -1;
	c->keep_alive_n++;
	if (c->keep_alive_n == c->conf->max_keep_alive_reqs)
		return -1;
	cl_reset(c);
	return 0;
}

static const char codestr[][15] = {
	"AHFILTER_DONE",
	"AHFILTER_FWD",
	"AHFILTER_BACK",
	"AHFILTER_ASYNC",
	"AHFILTER_SKIP",
	"AHFILTER_ERR",
	"AHFILTER_FIN",
};

/** Call the current filter */
static int cl_filter_call(alphahttpd_client *c, int i)
{
	const struct alphahttpd_filter *f = c->conf->filters[i];
	int r;

	if (!c->mdata[i].opened && !c->mdata[i].done) {
		cl_extralog(c, "filter %u: opening", i);
		r = f->open(c);
		(void)codestr;
		cl_extralog(c, "  filter %u returned: %s", i, codestr[r]);
		if (r == AHFILTER_SKIP || r == AHFILTER_ERR) {
			c->mdata[i].done = 1;
			c->output = c->input;
		} else {
			c->mdata[i].opened = 1;
		}
	}

	if (!c->mdata[i].done) {
		cl_extralog(c, "filter %u: input:%L", i, c->input.len);
		r = f->process(c);
		cl_extralog(c, "  filter %u returned: %s  output:%L", i, codestr[r], c->output.len);
	} else {
		r = (!c->chain_back) ? AHFILTER_FWD : AHFILTER_BACK;
		c->output = c->input;
	}
	return r;
}

static void cl_filters_process(alphahttpd_client *c)
{
	int i = c->imod, r;
	for (;;) {
		r = cl_filter_call(c, i);

		switch (r) {
		case AHFILTER_SKIP:
		case AHFILTER_DONE:
			c->mdata[i].done = 1;
			// fallthrough

		case AHFILTER_FWD:
			c->input = c->output;
			ffstr_null(&c->output);
			c->chain_back = 0;
			i++;
			if (c->conf->filters[i] == NULL) {
				if (0 != cl_keepalive(c))
					goto end;
				i = 0;
			}
			break;

		case AHFILTER_BACK:
			if (i == 0) {
				FF_ASSERT(0);
				break;
			}
			ffstr_null(&c->input);
			c->chain_back = 1;
			i--;
			break;

		case AHFILTER_ASYNC:
			c->imod = i;
			return;

		case AHFILTER_ERR:
		case AHFILTER_FIN:
			goto end;

		default:
			FF_ASSERT(0);
			goto end;
		}
	}

end:
	cl_destroy(c);
}
