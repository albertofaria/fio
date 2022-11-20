
static int fio_blkio_commit(struct thread_data *td)
{
	const struct fio_blkio_options *options = td->eo;
	struct fio_blkio_data *data = td->io_ops_data;

	/*
	 * If the wait mode is BLKIO_WAIT_MODE_DO_IO, we don't call
	 * blkioq_do_io() here since we are going to do so in getevents(), which
	 * should be called immediately after we return from this function.
	 *
	 * This may cause submission latency to be reported erroneously (TODO:
	 * check this), but since proper use of the libblkio API in this case is
	 * to invoke blkioq_do_io() both for submitting and waiting, it may be
	 * argued that submission latencies are not meaningful.
	 *
	 * Regardless, since this callback can be invoked superfluously, we
	 * check to make sure we avoid calling blkioq_do_io() when there are no
	 * requests awaiting submission.
	 */

	if (options->wait_mode != BLKIO_WAIT_MODE_DO_IO && data->needs_submit) {
		if (blkioq_do_io(data->q, NULL, 0, 0, NULL) < 0) {
			fio_blkio_log_err(blkioq_do_io);
			return -1;
		}

		data->needs_submit = false;
	}

	return 0;

	/*
	 * TODO: Might need to call io_u_queued on all submitted io_u's and
	 * conditionally set their io_u->issue_time. The io_uring engine does
	 * this, but several other async engines don't.
	 */
}
