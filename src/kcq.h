/** alphahttpd: kernel call queue workers
2022, Simon Zolin */

int kcq_init()
{
	if (NULL == (boss->kcq_sq = ffrq_alloc(ahd_conf->aconf.server.max_connections))) {
		return -1;
	}
	if (!ahd_conf->aconf.server.polling_mode
		&& FFSEM_NULL == (boss->kcq_sem = ffsem_open(NULL, 0, 0))) {
		return -1;
	}
	if (NULL == ffvec_allocT(&boss->kcq_workers, ahd_conf->kcall_workers, ffthread)) {
		return -1;
	}
	boss->kcq_workers.len = ahd_conf->kcall_workers;
	return 0;
}

void kcq_destroy()
{
	FFINT_WRITEONCE(boss->kcq_stop, 1);
	ffthread *it;

	if (boss->kcq_sem != FFSEM_NULL) {
		dbglog("stopping kcall workers");
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
	dbglog("entering kcall loop");
	while (!FFINT_READONCE(boss->kcq_stop)) {
		ffkcallq_process_sq(boss->kcq_sq);
		if (!ahd_conf->aconf.server.polling_mode)
			ffsem_wait(boss->kcq_sem, -1);
	}
	dbglog("left kcall loop");
	return 0;
}

int kcq_start()
{
	ffthread *it;
	FFSLICE_WALK(&boss->kcq_workers, it) {
		if (FFTHREAD_NULL == (*it = ffthread_create(kcq_worker, boss, 0))) {
			syserrlog("thread create");
			return -1;
		}
	}
	return 0;
}

void kcq_set(void *opaque, struct ffkcallqueue *kcq)
{
	kcq->sq = boss->kcq_sq;
	kcq->sem = boss->kcq_sem;
}
