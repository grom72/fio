/*
 * librpma: IO engine that uses PMDK librpma to read and write data
 *
 * Copyright 2020, Intel Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License,
 * version 2 as published by the Free Software Foundation..
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include "../fio.h"
#include "../hash.h"
#include "../optgroup.h"

#include "librpma_common.h"

#include <libpmem.h>
#include <librpma.h>

/* client's and server's common */

#define rpma_td_verror(td, err, func) \
	td_vmsg((td), (err), rpma_err_2str(err), (func))

/* ceil(a / b) = (a + b - 1) / b */
#define CEIL(a, b) (((a) + (b) - 1) / (b))

/*
 * Limited by the maximum length of the private data
 * for rdma_connect() in case of RDMA_PS_TCP (28 bytes).
 */
#define DESCRIPTORS_MAX_SIZE 27

struct workspace {
	uint8_t mr_desc_size;	/* size of mr_desc in descriptors[] */
	/* buffer containing mr_desc */
	char descriptors[DESCRIPTORS_MAX_SIZE];
};

/* client side implementation */

struct client_options {
	/*
	 * FIO considers .off1 == 0 absent so the first meaningful field has to
	 * have padding ahead of it.
	 */
	void *pad;
	char *hostname;
	char *port;
};

static struct fio_option fio_client_options[] = {
	{
		.name	= "hostname",
		.lname	= "rpma_client hostname",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct client_options, hostname),
		.help	= "IP address the server is listening on",
		.def    = "",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBRPMA,
	},
	{
		.name	= "port",
		.lname	= "rpma_client port",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct client_options, port),
		.help	= "port the server is listening on",
		.def    = "7204",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBRPMA,
	},
	{
		.name	= NULL,
	},
};

struct client_data {
	struct rpma_peer *peer;
	struct rpma_conn *conn;
	enum rpma_flush_type flush_type;

	/* aligned td->orig_buffer */
	char *orig_buffer_aligned;

	/* ious's base address memory registration (cd->orig_buffer_aligned) */
	struct rpma_mr_local *orig_mr;

	/* a server's memory representation */
	struct rpma_mr_remote *server_mr;

	/* remote workspace description */
	size_t ws_size;

	/* in-memory queues */
	struct io_u **io_us_queued;
	int io_u_queued_nr;
	struct io_u **io_us_flight;
	int io_u_flight_nr;
	struct io_u **io_us_completed;
	int io_u_completed_nr;
};

static int client_init(struct thread_data *td)
{
	struct client_options *o = td->eo;
	struct client_data *cd;
	struct ibv_context *dev = NULL;
	struct rpma_conn_cfg *cfg = NULL;
	char port_td[LIBRPMA_COMMON_PORT_STR_LEN_MAX];
	struct rpma_conn_req *req = NULL;
	enum rpma_conn_event event;
	uint32_t cq_size;
	struct rpma_conn_private_data pdata;
	struct workspace *ws;
	size_t server_mr_size;
	int remote_flush_type;
	struct rpma_peer_cfg *pcfg = NULL;
	unsigned int sq_size;
	int ret = 1;

	/* configure logging thresholds to see more details */
	rpma_log_set_threshold(RPMA_LOG_THRESHOLD, RPMA_LOG_LEVEL_INFO);
	rpma_log_set_threshold(RPMA_LOG_THRESHOLD_AUX, RPMA_LOG_LEVEL_ERROR);

	/* not supported readwrite = trim / randtrim / trimwrite */
	if (td_trim(td)) {
		log_err("Not supported mode.\n");
		return 1;
	}

	/* allocate client's data */
	cd = calloc(1, sizeof(struct client_data));
	if (cd == NULL) {
		td_verror(td, errno, "calloc");
		return 1;
	}

	/* allocate all in-memory queues */
	cd->io_us_queued = calloc(td->o.iodepth, sizeof(struct io_u *));
	if (cd->io_us_queued == NULL) {
		td_verror(td, errno, "calloc");
		goto err_free_cd;
	}

	cd->io_us_flight = calloc(td->o.iodepth, sizeof(struct io_u *));
	if (cd->io_us_flight == NULL) {
		td_verror(td, errno, "calloc");
		goto err_free_io_us_queued;
	}

	cd->io_us_completed = calloc(td->o.iodepth, sizeof(struct io_u *));
	if (cd->io_us_completed == NULL) {
		td_verror(td, errno, "calloc");
		goto err_free_io_us_flight;
	}

	/* obtain an IBV context for a remote IP address */
	ret = rpma_utils_get_ibv_context(o->hostname,
				RPMA_UTIL_IBV_CONTEXT_REMOTE,
				&dev);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_utils_get_ibv_context");
		goto err_free_io_us_completed;
	}

	/* create a connection configuration object */
	ret = rpma_conn_cfg_new(&cfg);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_conn_cfg_new");
		goto err_free_io_us_completed;
	}

	/*
	 * Calculate the required queue sizes where:
	 * - the send queue (SQ) has to be big enough to accommodate
	 *   all io_us (WRITEs) and all flush requests (FLUSHes)
	 * - the completion queue (CQ) has to be big enough to accommodate all
	 *   success and error completions (cq_size = sq_size)
	 */
	if (td_random(td) || td_rw(td)) {
		/*
		 * sq_size = max(rand_read_sq_size, rand_write_sq_size)
		 * where rand_read_sq_size < rand_write_sq_size because read
		 * does not require flush afterwards
		 * rand_write_sq_size = N * (WRITE + FLUSH)
		 *
		 * Note: rw is no different from random write since having
		 * interleaved reads with writes in extreme forces you to flush
		 * as often as when the writes are random.
		 */
		sq_size = 2 * td->o.iodepth;
	} else if (td_write(td)) {
		/* sequential TD_DDIR_WRITE only */
		if (td->o.sync_io) {
			sq_size = 2; /* WRITE + FLUSH */
		} else {
			/*
			 * N * WRITE + B * FLUSH where:
			 * - B == ceil(iodepth / iodepth_batch)
			 *   which is the number of batches for N writes
			 */
			sq_size = td->o.iodepth +
				CEIL(td->o.iodepth, td->o.iodepth_batch);
		}
	} else {
		/* TD_DDIR_READ only */
		if (td->o.sync_io) {
			sq_size = 1; /* READ */
		} else {
			sq_size = td->o.iodepth; /* N x READ */
		}
	}
	cq_size = sq_size;

	/* apply queue sizes */
	ret = rpma_conn_cfg_set_sq_size(cfg, sq_size);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_conn_cfg_set_sq_size");
		goto err_cfg_delete;
	}
	ret = rpma_conn_cfg_set_cq_size(cfg, cq_size);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_conn_cfg_set_cq_size");
		goto err_cfg_delete;
	}

	/* create a new peer object */
	ret = rpma_peer_new(dev, &cd->peer);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_peer_new");
		goto err_cfg_delete;
	}

	/* create a connection request */
	if ((ret = librpma_common_td_port(o->port, td, port_td)))
		goto err_peer_delete;
	ret = rpma_conn_req_new(cd->peer, o->hostname, port_td, cfg, &req);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_conn_req_new");
		goto err_peer_delete;
	}

	ret = rpma_conn_cfg_delete(&cfg);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_conn_cfg_delete");
		goto err_peer_delete;
	}

	/* connect the connection request and obtain the connection object */
	ret = rpma_conn_req_connect(&req, NULL, &cd->conn);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_conn_req_connect");
		goto err_req_delete;
	}

	/* wait for the connection to establish */
	ret = rpma_conn_next_event(cd->conn, &event);
	if (ret) {
		goto err_conn_delete;
	} else if (event != RPMA_CONN_ESTABLISHED) {
		log_err(
			"rpma_conn_next_event returned an unexptected event: (%s != RPMA_CONN_ESTABLISHED)\n",
			rpma_utils_conn_event_2str(event));
		goto err_conn_delete;
	}

	/* get the connection's private data sent from the server */
	if ((ret = rpma_conn_get_private_data(cd->conn, &pdata)))
		goto err_conn_delete;

	ws = pdata.ptr;

	/* create the server's memory representation */
	if ((ret = rpma_mr_remote_from_descriptor(&ws->descriptors[0],
			ws->mr_desc_size, &cd->server_mr)))
		goto err_conn_delete;

	/* get the total size of the shared server memory */
	if ((ret = rpma_mr_remote_get_size(cd->server_mr, &server_mr_size))) {
		rpma_td_verror(td, ret, "rpma_mr_remote_get_size");
		goto err_conn_delete;
	}

	/* get flush type of the remote node */
	if ((ret = rpma_mr_remote_get_flush_type(cd->server_mr, &remote_flush_type))) {
		rpma_td_verror(td, ret, "rpma_mr_remote_get_flush_type");
		goto err_conn_delete;
	}

	cd->flush_type = (remote_flush_type & RPMA_MR_USAGE_FLUSH_TYPE_PERSISTENT) ?
		RPMA_FLUSH_TYPE_PERSISTENT : RPMA_FLUSH_TYPE_VISIBILITY;

	if (cd->flush_type == RPMA_FLUSH_TYPE_PERSISTENT) {
		/* configure peer's direct write to pmem support */
		ret = rpma_peer_cfg_new(&pcfg);
		if (ret) {
			rpma_td_verror(td, ret, "rpma_peer_cfg_new");
			goto err_conn_delete;
		}

		ret = rpma_peer_cfg_set_direct_write_to_pmem(pcfg, true);
		if (ret) {
			rpma_td_verror(td, ret, "rpma_peer_cfg_set_direct_write_to_pmem");
			goto peer_cfg_delete;
		}

		ret = rpma_conn_apply_remote_peer_cfg(cd->conn, pcfg);
		if (ret) {
			rpma_td_verror(td, ret, "rpma_conn_apply_remote_peer_cfg");
			goto peer_cfg_delete;
		}

		(void) rpma_peer_cfg_delete(&pcfg);
	}

	cd->ws_size = server_mr_size;
	td->io_ops_data = cd;

	return 0;

peer_cfg_delete:
	(void) rpma_peer_cfg_delete(&pcfg);

err_conn_delete:
	(void) rpma_conn_disconnect(cd->conn);
	(void) rpma_conn_delete(&cd->conn);

err_req_delete:
	if (req)
		(void) rpma_conn_req_delete(&req);
err_peer_delete:
	(void) rpma_peer_delete(&cd->peer);

err_cfg_delete:
	(void) rpma_conn_cfg_delete(&cfg);

err_free_io_us_completed:
	free(cd->io_us_completed);

err_free_io_us_flight:
	free(cd->io_us_flight);

err_free_io_us_queued:
	free(cd->io_us_queued);

err_free_cd:
	free(cd);

	return 1;
}

static int client_post_init(struct thread_data *td)
{
	struct client_data *cd =  td->io_ops_data;
	size_t io_us_size;
	int ret;

	/*
	 * td->orig_buffer is not aligned. The engine requires aligned io_us
	 * so FIO alignes up the address using the formula below.
	 */
	cd->orig_buffer_aligned = PTR_ALIGN(td->orig_buffer, page_mask) +
			td->o.mem_align;

	/*
	 * td->orig_buffer_size beside the space really consumed by io_us
	 * has paddings which can be omitted for the memory registration.
	 */
	io_us_size = (unsigned long long)td_max_bs(td) *
			(unsigned long long)td->o.iodepth;

	if ((ret = rpma_mr_reg(cd->peer, cd->orig_buffer_aligned, io_us_size,
			RPMA_MR_USAGE_READ_DST | RPMA_MR_USAGE_READ_SRC |
			RPMA_MR_USAGE_WRITE_DST | RPMA_MR_USAGE_WRITE_SRC |
			RPMA_MR_USAGE_FLUSH_TYPE_PERSISTENT,
			&cd->orig_mr)))
		rpma_td_verror(td, ret, "rpma_mr_reg");
	return ret;
}

static void client_cleanup(struct thread_data *td)
{
	struct client_data *cd = td->io_ops_data;
	enum rpma_conn_event ev;
	int ret;

	if (cd == NULL)
		return;

	/* delete the iou's memory registration */
	if ((ret = rpma_mr_dereg(&cd->orig_mr)))
		rpma_td_verror(td, ret, "rpma_mr_dereg");

	/* delete the iou's memory registration */
	if ((ret = rpma_mr_remote_delete(&cd->server_mr)))
		rpma_td_verror(td, ret, "rpma_mr_remote_delete");

	/* initiate disconnection */
	if ((ret = rpma_conn_disconnect(cd->conn)))
		rpma_td_verror(td, ret, "rpma_conn_disconnect");

	/* wait for disconnection to end up */
	if ((ret = rpma_conn_next_event(cd->conn, &ev))) {
		rpma_td_verror(td, ret, "rpma_conn_next_event");
	} else if (ev != RPMA_CONN_CLOSED) {
		log_err(
			"client_cleanup received an unexpected event (%s != RPMA_CONN_CLOSED)\n",
			rpma_utils_conn_event_2str(ev));
	}

	/* delete the connection */
	if ((ret = rpma_conn_delete(&cd->conn)))
		rpma_td_verror(td, ret, "rpma_conn_delete");

	/* delete the peer */
	if ((ret = rpma_peer_delete(&cd->peer)))
		rpma_td_verror(td, ret, "rpma_peer_delete");

	/* free the software queues */
	free(cd->io_us_queued);
	free(cd->io_us_flight);
	free(cd->io_us_completed);

	/* free the client's data */
	free(td->io_ops_data);
}

static int client_get_file_size(struct thread_data *td, struct fio_file *f)
{
	struct client_data *cd = td->io_ops_data;

	f->real_file_size = cd->ws_size;
	fio_file_set_size_known(f);

	return 0;
}

static int client_open_file(struct thread_data *td, struct fio_file *f)
{
	/* NOP */
	return 0;
}

static int client_close_file(struct thread_data *td, struct fio_file *f)
{
	/* NOP */
	return 0;
}

static inline int client_io_read(struct thread_data *td, struct io_u *io_u, int flags)
{
	struct client_data *cd = td->io_ops_data;
	size_t dst_offset = (char *)(io_u->xfer_buf) - cd->orig_buffer_aligned;
	size_t src_offset = io_u->offset;
	int ret = rpma_read(cd->conn,
			cd->orig_mr, dst_offset,
			cd->server_mr, src_offset,
			io_u->xfer_buflen,
			flags,
			(void *)(uintptr_t)io_u->index);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_read");
		return -1;
	}

	return 0;
}

static inline int client_io_write(struct thread_data *td, struct io_u *io_u, int flags)
{
	struct client_data *cd = td->io_ops_data;
	size_t src_offset = (char *)(io_u->xfer_buf) - cd->orig_buffer_aligned;
	size_t dst_offset = io_u->offset;

	int ret = rpma_write(cd->conn,
			cd->server_mr, dst_offset,
			cd->orig_mr, src_offset,
			io_u->xfer_buflen,
			flags,
			(void *)(uintptr_t)io_u->index);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_write");
		return -1;
	}

	return 0;
}

static inline int client_io_flush(struct thread_data *td,
		struct io_u *first_io_u, struct io_u *last_io_u,
		unsigned long long int len)
{
	struct client_data *cd = td->io_ops_data;
	size_t dst_offset = first_io_u->offset;

	int ret = rpma_flush(cd->conn, cd->server_mr, dst_offset, len,
		cd->flush_type, RPMA_F_COMPLETION_ALWAYS,
		(void *)(uintptr_t)last_io_u->index);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_flush");
		return -1;
	}

	return 0;
}

static enum fio_q_status client_queue_sync(struct thread_data *td,
					  struct io_u *io_u)
{
	struct client_data *cd = td->io_ops_data;
	struct rpma_completion cmpl;
	/* io_u->index of completed io_u (cmpl.op_context) */
	unsigned int io_u_index;
	int ret;

	/* execute io_u */
	if (io_u->ddir == DDIR_READ) {
		/* post an RDMA read operation */
		if ((ret = client_io_read(td, io_u, RPMA_F_COMPLETION_ALWAYS)))
			goto err;
	} else if (io_u->ddir == DDIR_WRITE) {
		/* post an RDMA write operation */
		if ((ret = client_io_write(td, io_u, RPMA_F_COMPLETION_ON_ERROR)))
			goto err;
		if ((ret = client_io_flush(td, io_u, io_u, io_u->xfer_buflen)))
			goto err;
	} else {
		log_err("unsupported IO mode: %s\n", io_ddir_name(io_u->ddir));
		goto err;
	}

	do {
		/* get a completion */
		ret = rpma_conn_completion_get(cd->conn, &cmpl);
		if (ret == 0) {
			/* if io_u has completed with an error */
			if (cmpl.op_status != IBV_WC_SUCCESS)
				goto err;
		} else if (ret != RPMA_E_NO_COMPLETION) {
			/* an error occurred */
			rpma_td_verror(td, ret, "rpma_conn_completion_get");
			goto err;
		}
	} while (ret == RPMA_E_NO_COMPLETION);

	memcpy(&io_u_index, &cmpl.op_context, sizeof(unsigned int));
	if (io_u->index != io_u_index) {
		log_err(
			"no matching io_u for received completion found (io_u_index=%u)\n",
			io_u_index);
		goto err;
	}

	return FIO_Q_COMPLETED;

err:
	io_u->error = -1;
	return FIO_Q_COMPLETED;
}

static enum fio_q_status client_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct client_data *cd = td->io_ops_data;

	if (cd->io_u_queued_nr == (int)td->o.iodepth)
		return FIO_Q_BUSY;

	if (td->o.sync_io)
		return client_queue_sync(td, io_u);

	/* io_u -> queued[] */
	cd->io_us_queued[cd->io_u_queued_nr] = io_u;
	cd->io_u_queued_nr++;

	return FIO_Q_QUEUED;
}

static int client_commit(struct thread_data *td)
{
	struct client_data *cd = td->io_ops_data;
	int flags = RPMA_F_COMPLETION_ON_ERROR;
	struct timespec now;
	bool fill_time;
	int ret;
	int i;
	struct io_u *flush_first_io_u = NULL;
	unsigned long long int flush_len = 0;

	if (!cd->io_us_queued)
		return -1;

	/* execute all io_us from queued[] */
	for (i = 0; i < cd->io_u_queued_nr; i++) {
		struct io_u *io_u = cd->io_us_queued[i];

		if (io_u->ddir == DDIR_READ) {
			if (i + 1 == cd->io_u_queued_nr || cd->io_us_queued[i + 1]->ddir == DDIR_WRITE)
				flags = RPMA_F_COMPLETION_ALWAYS;
			/* post an RDMA read operation */
			if ((ret = client_io_read(td, io_u, flags)))
				return -1;
		} else if (io_u->ddir == DDIR_WRITE) {
			/* post an RDMA write operation */
			ret = client_io_write(td, io_u, RPMA_F_COMPLETION_ON_ERROR);
			if (ret)
				return -1;

			/* cache the first io_u in the sequence */
			if (flush_first_io_u == NULL)
				flush_first_io_u = io_u;

			/*
			 * the flush length is the sum of all io_u's creating
			 * the sequence
			 */
			flush_len += io_u->xfer_buflen;

			/*
			 * if io_u's are random the rpma_flush is required after
			 * each one of them
			 */
			if (!td_random(td)) {
				/* when the io_u's are sequential and
				 * the current io_u is not the last one and
				 * the next one is also a write operation
				 * the flush can be postponed by one io_u and
				 * cover all of them which build a continuous
				 * sequence
				 */
				if (i + 1 < cd->io_u_queued_nr &&
						cd->io_us_queued[i + 1]->ddir == DDIR_WRITE)
					continue;
			}

			/* flush all writes which build a continuous sequence */
			ret = client_io_flush(td, flush_first_io_u, io_u, flush_len);
			if (ret)
				return -1;

			/*
			 * reset the flush parameters in preparation for
			 * the next one
			 */
			flush_first_io_u = NULL;
			flush_len = 0;
		} else {
			log_err("unsupported IO mode: %s\n", io_ddir_name(io_u->ddir));
			return -1;
		}
	}

	if ((fill_time = fio_fill_issue_time(td)))
		fio_gettime(&now, NULL);

	/* move executed io_us from queued[] to flight[] */
	for (i = 0; i < cd->io_u_queued_nr; i++) {
		struct io_u *io_u = cd->io_us_queued[i];

		/* FIO does not do this if the engine is asynchronous */
		if (fill_time)
			memcpy(&io_u->issue_time, &now, sizeof(now));

		/* move executed io_us from queued[] to flight[] */
		cd->io_us_flight[cd->io_u_flight_nr] = io_u;
		cd->io_u_flight_nr++;

		/*
		 * FIO says:
		 * If an engine has the commit hook it has to call io_u_queued() itself.
		 */
		io_u_queued(td, io_u);
	}

	/* FIO does not do this if an engine has the commit hook. */
	io_u_mark_submit(td, cd->io_u_queued_nr);
	cd->io_u_queued_nr = 0;

	return 0;
}

/*
 * RETURN VALUE
 * - > 0  - a number of completed io_us
 * -   0  - when no complicitions received
 * - (-1) - when an error occurred
 */
static int client_getevent_process(struct thread_data *td)
{
	struct client_data *cd = td->io_ops_data;
	struct rpma_completion cmpl;
	unsigned int io_us_error = 0;
	/* io_u->index of completed io_u (cmpl.op_context) */
	unsigned int io_u_index;
	/* # of completed io_us */
	int cmpl_num = 0;
	/* helpers */
	struct io_u *io_u;
	int i;
	int ret;

	/* get a completion */
	if ((ret = rpma_conn_completion_get(cd->conn, &cmpl))) {
		/* lack of completion is not an error */
		if (ret == RPMA_E_NO_COMPLETION)
			return 0;

		/* an error occurred */
		rpma_td_verror(td, ret, "rpma_conn_completion_get");
		return -1;
	}

	/* if io_us has completed with an error */
	if (cmpl.op_status != IBV_WC_SUCCESS)
		io_us_error = cmpl.op_status;

	/* look for an io_u being completed */
	memcpy(&io_u_index, &cmpl.op_context, sizeof(unsigned int));
	for (i = 0; i < cd->io_u_flight_nr; ++i) {
		if (cd->io_us_flight[i]->index == io_u_index) {
			cmpl_num = i + 1;
			break;
		}
	}

	/* if no matching io_u has been found */
	if (cmpl_num == 0) {
		log_err(
			"no matching io_u for received completion found (io_u_index=%u)\n",
			io_u_index);
		return -1;
	}

	/* move completed io_us to the completed in-memory queue */
	for (i = 0; i < cmpl_num; ++i) {
		/* get and prepare io_u */
		io_u = cd->io_us_flight[i];
		io_u->error = io_us_error;

		/* append to the queue */
		cd->io_us_completed[cd->io_u_completed_nr] = io_u;
		cd->io_u_completed_nr++;
	}

	/* remove completed io_us from the flight queue */
	for (i = cmpl_num; i < cd->io_u_flight_nr; ++i)
		cd->io_us_flight[i - cmpl_num] = cd->io_us_flight[i];
	cd->io_u_flight_nr -= cmpl_num;

	return cmpl_num;
}

static int client_getevents(struct thread_data *td, unsigned int min,
				unsigned int max, const struct timespec *t)
{
	/* total # of completed io_us */
	int cmpl_num_total = 0;
	/* # of completed io_us from a single event */
	int cmpl_num;

	do {
		cmpl_num = client_getevent_process(td);
		if (cmpl_num > 0) {
			/* new completions collected */
			cmpl_num_total += cmpl_num;
		} else if (cmpl_num == 0) {
			if (cmpl_num_total >= min)
				break;

			/* To reduce CPU consumption one can use
			 * the rpma_conn_completion_wait() function.
			 * Note this greatly increase the latency
			 * and make the results less stable.
			 * The bandwidth stays more or less the same.
			 */
		} else {
			/* an error occurred */
			return -1;
		}
	} while (cmpl_num_total < max);

	return cmpl_num_total;
}

static struct io_u *client_event(struct thread_data *td, int event)
{
	struct client_data *cd = td->io_ops_data;
	struct io_u *io_u;
	int i;

	/* get the first io_u from the queue */
	io_u = cd->io_us_completed[0];

	/* remove the first io_u from the queue */
	for (i = 1; i < cd->io_u_completed_nr; ++i)
		cd->io_us_completed[i - 1] = cd->io_us_completed[i];
	cd->io_u_completed_nr--;

	dprint_io_u(io_u, "client_event");

	return io_u;
}

static char *client_errdetails(struct io_u *io_u)
{
	/* get the string representation of an error */
	enum ibv_wc_status status = io_u->error;
	const char *status_str = ibv_wc_status_str(status);

	/* allocate and copy the error string representation */
	char *details = malloc(strlen(status_str) + 1);
	strcpy(details, status_str);

	/* FIO frees the returned string when it becomes obsolete */
	return details;
}

FIO_STATIC struct ioengine_ops ioengine_client = {
	.name			= "librpma_client",
	.version		= FIO_IOOPS_VERSION,
	.init			= client_init,
	.post_init		= client_post_init,
	.get_file_size		= client_get_file_size,
	.open_file		= client_open_file,
	.queue			= client_queue,
	.commit			= client_commit,
	.getevents		= client_getevents,
	.event			= client_event,
	.errdetails		= client_errdetails,
	.close_file		= client_close_file,
	.cleanup		= client_cleanup,
	/* XXX flags require consideration */
	.flags			= FIO_DISKLESSIO | FIO_UNIDIR | FIO_PIPEIO,
	.options		= fio_client_options,
	.option_struct_size	= sizeof(struct client_options),
};

/* server side implementation */

struct server_options {
	/*
	 * FIO considers .off1 == 0 absent so the first meaningful field has to
	 * have padding ahead of it.
	 */
	void *pad;
	char *bindname;
	char *port;
};

static struct fio_option fio_server_options[] = {
	{
		.name	= "bindname",
		.lname	= "rpma_server bindname",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct server_options, bindname),
		.help	= "IP address to listen on for incoming connections",
		.def    = "",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBRPMA,
	},
	{
		.name	= "port",
		.lname	= "rpma_server port",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct server_options, port),
		.help	= "port to listen on for incoming connections",
		.def    = "7204",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBRPMA,
	},
	{
		.name	= NULL,
	},
};

struct server_data {
	struct rpma_peer *peer;

	/* resources of an incoming connection */
	struct rpma_conn *conn;

	/* memory buffer */
	char *mem_ptr;

	/* size of the mapped persistent memory */
	size_t size_mmap;
};

static int server_init(struct thread_data *td)
{
	struct server_options *o = td->eo;
	struct server_data *sd;
	struct ibv_context *dev = NULL;
	int ret = 1;

	/* configure logging thresholds to see more details */
	rpma_log_set_threshold(RPMA_LOG_THRESHOLD, RPMA_LOG_LEVEL_INFO);
	rpma_log_set_threshold(RPMA_LOG_THRESHOLD_AUX, RPMA_LOG_LEVEL_ERROR);

	/* allocate server's data */
	sd = calloc(1, sizeof(struct server_data));
	if (sd == NULL) {
		td_verror(td, errno, "calloc");
		return 1;
	}

	/* obtain an IBV context for a remote IP address */
	ret = rpma_utils_get_ibv_context(o->bindname,
				RPMA_UTIL_IBV_CONTEXT_LOCAL,
				&dev);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_utils_get_ibv_context");
		goto err_free_sd;
	}

	/* create a new peer object */
	ret = rpma_peer_new(dev, &sd->peer);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_peer_new");
		goto err_free_sd;
	}

	td->io_ops_data = sd;

	return 0;

err_free_sd:
	free(sd);

	return 1;
}

static void server_cleanup(struct thread_data *td)
{
	struct server_data *sd =  td->io_ops_data;
	int ret;

	if (sd == NULL)
		return;

	/* free the peer */
	if ((ret = rpma_peer_delete(&sd->peer)))
		rpma_td_verror(td, ret, "rpma_peer_delete");
}

static char *server_allocate_pmem(struct thread_data *td, const char *filename, size_t size)
{
	struct server_data *sd =  td->io_ops_data;
	size_t size_mmap = 0;
	char *mem_ptr = NULL;
	int is_pmem = 0;

	/* XXX assuming size is page aligned */
	size_t ws_offset = (td->thread_number - 1) * size;

	if (!filename) {
		log_err("fio: filename is not set\n");
		return NULL;
	}

	/* map the file */
	mem_ptr = pmem_map_file(filename, 0 /* len */, 0 /* flags */,
			0 /* mode */, &size_mmap, &is_pmem);
	if (mem_ptr == NULL) {
		log_err("fio: pmem_map_file(%s) failed\n", filename);
		/* pmem_map_file() sets errno on failure */
		td_verror(td, errno, "pmem_map_file");
		return NULL;
	}

	/* pmem is expected */
	if (!is_pmem) {
		log_err("fio: %s is not located in persistent memory\n", filename);
		goto err_unmap;
	}

	/* check size of allocated persistent memory */
	if (size_mmap < ws_offset + size) {
		log_err(
			"fio: %s is too small to handle so many threads (%zu < %zu)\n",
			filename, size_mmap, ws_offset + size);
		goto err_unmap;
	}

	sd->mem_ptr = mem_ptr;
	sd->size_mmap = size_mmap;

	return mem_ptr + ws_offset;

err_unmap:
	(void) pmem_unmap(mem_ptr, size_mmap);
	return NULL;
}

static char *server_allocate_dram(struct thread_data *td, size_t size)
{
	struct server_data *sd =  td->io_ops_data;
	char *mem_ptr = NULL;
	int ret;

	if ((ret = posix_memalign((void **)&mem_ptr, page_size, size))) {
		log_err("fio: posix_memalign() failed\n");
		td_verror(td, ret, "posix_memalign");
		return NULL;
	}

	sd->mem_ptr = mem_ptr;
	sd->size_mmap = 0;

	return mem_ptr;
}

static void server_free(struct thread_data *td)
{
	struct server_data *sd = td->io_ops_data;

	if (sd->size_mmap)
		(void) pmem_unmap(sd->mem_ptr, sd->size_mmap);
	else
		free(sd->mem_ptr);
}

static int server_open_file(struct thread_data *td, struct fio_file *f)
{
	struct server_data *sd =  td->io_ops_data;
	struct server_options *o = td->eo;
	enum rpma_conn_event conn_event = RPMA_CONN_UNDEFINED;
	struct rpma_mr_local *mr;
	char *mem_ptr = NULL;
	size_t mr_desc_size;
	size_t mem_size = td->o.size;
	struct rpma_conn_private_data pdata;
	struct workspace ws;
	struct rpma_conn_req *conn_req;
	struct rpma_conn *conn;
	char port_td[LIBRPMA_COMMON_PORT_STR_LEN_MAX];
	struct rpma_ep *ep;
	int ret = 1;

	if (!f->file_name) {
		log_err("fio: filename is not set\n");
		return 1;
	}

	if (strcmp(f->file_name, "malloc") == 0) {
		/* allocation from DRAM using posix_memalign() */
		mem_ptr = server_allocate_dram(td, mem_size);
	} else {
		/* allocation from PMEM using pmem_map_file() */
		mem_ptr = server_allocate_pmem(td, f->file_name, mem_size);
	}

	if (mem_ptr == NULL)
		return 1;

	f->real_file_size = mem_size;

	ret = rpma_mr_reg(sd->peer, mem_ptr, mem_size,
			RPMA_MR_USAGE_READ_DST | RPMA_MR_USAGE_READ_SRC |
			RPMA_MR_USAGE_WRITE_DST | RPMA_MR_USAGE_WRITE_SRC |
			RPMA_MR_USAGE_FLUSH_TYPE_PERSISTENT,
			&mr);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_mr_reg");
		goto err_free;
	}

	/* get size of the memory region's descriptor */
	ret = rpma_mr_get_descriptor_size(mr, &mr_desc_size);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_mr_get_descriptor_size");
		goto err_mr_dereg;
	}

	/* verify size of the memory region's descriptor */
	if (mr_desc_size > DESCRIPTORS_MAX_SIZE) {
		log_err("size of the memory region's descriptor is too big (max=%i)\n",
			DESCRIPTORS_MAX_SIZE);
		goto err_mr_dereg;
	}

	/* get the memory region's descriptor */
	ret = rpma_mr_get_descriptor(mr, &ws.descriptors[0]);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_mr_get_descriptor");
		goto err_mr_dereg;
	}

	/* prepare a workspace description */
	ws.mr_desc_size = mr_desc_size;
	pdata.ptr = &ws;
	pdata.len = sizeof(struct workspace);

	/* start a listening endpoint at addr:port */
	if ((ret = librpma_common_td_port(o->port, td, port_td)))
		goto err_mr_dereg;

	ret = rpma_ep_listen(sd->peer, o->bindname, port_td, &ep);
	if (ret) {
		rpma_td_verror(td, ret, "rpma_ep_listen");
		goto err_mr_dereg;
	}

	/* receive an incoming connection request */
	if ((ret = rpma_ep_next_conn_req(ep, NULL, &conn_req)))
		goto err_ep_shutdown;

	/* accept the connection request and obtain the connection object */
	if ((ret = rpma_conn_req_connect(&conn_req, &pdata, &conn)))
		goto err_req_delete;

	/* wait for the connection to be established */
	ret = rpma_conn_next_event(conn, &conn_event);
	if (ret)
		rpma_td_verror(td, ret, "rpma_conn_next_event");
	if (!ret && conn_event != RPMA_CONN_ESTABLISHED) {
		log_err("rpma_conn_next_event returned an unexptected event\n");
		ret = 1;
	}
	if (ret)
		goto err_conn_delete;

	/* end-point is no longer needed */
	(void) rpma_ep_shutdown(&ep);

	sd->conn = conn;

	FILE_SET_ENG_DATA(f, mr);

	return 0;

err_conn_delete:
	(void) rpma_conn_delete(&conn);

err_req_delete:
	(void) rpma_conn_req_delete(&conn_req);

err_ep_shutdown:
	(void) rpma_ep_shutdown(&ep);

err_mr_dereg:
	(void) rpma_mr_dereg(&mr);

err_free:
	server_free(td);

	return (ret != 0 ? ret : 1);
}

static int server_close_file(struct thread_data *td, struct fio_file *f)
{
	struct server_data *sd =  td->io_ops_data;
	enum rpma_conn_event conn_event = RPMA_CONN_UNDEFINED;
	struct rpma_mr_local *mr = FILE_ENG_DATA(f);
	int ret;
	int rv;

	/* wait for the connection to be closed */
	ret = rpma_conn_next_event(sd->conn, &conn_event);
	if (!ret && conn_event != RPMA_CONN_CLOSED) {
		log_err("rpma_conn_next_event returned an unexptected event\n");
		rv = 1;
	}

	if ((ret = rpma_conn_disconnect(sd->conn))) {
		rpma_td_verror(td, ret, "rpma_conn_disconnect");
		rv |= ret;
	}

	if ((ret = rpma_conn_delete(&sd->conn))) {
		rpma_td_verror(td, ret, "rpma_conn_delete");
		rv |= ret;
	}

	if ((ret = rpma_mr_dereg(&mr))) {
		rpma_td_verror(td, ret, "rpma_mr_dereg");
		rv |= ret;
	}

	server_free(td);

	FILE_SET_ENG_DATA(f, NULL);

	return ret;
}

static enum fio_q_status server_queue(struct thread_data *td,
					struct io_u *io_u)
{
	return FIO_Q_COMPLETED;
}

static int server_invalidate(struct thread_data *td, struct fio_file *file)
{
	/* NOP */
	return 0;
}

FIO_STATIC struct ioengine_ops ioengine_server = {
	.name			= "librpma_server",
	.version		= FIO_IOOPS_VERSION,
	.init			= server_init,
	.open_file		= server_open_file,
	.close_file		= server_close_file,
	.queue			= server_queue,
	.invalidate		= server_invalidate,
	.cleanup		= server_cleanup,
	.flags			= FIO_SYNCIO | FIO_NOEXTEND | FIO_FAKEIO |
				  FIO_NOSTATS,
	.options		= fio_server_options,
	.option_struct_size	= sizeof(struct server_options),
};

/* register both engines */

static void fio_init fio_librpma_register(void)
{
	register_ioengine(&ioengine_client);
	register_ioengine(&ioengine_server);
}

static void fio_exit fio_librpma_unregister(void)
{
	unregister_ioengine(&ioengine_client);
	unregister_ioengine(&ioengine_server);
}
