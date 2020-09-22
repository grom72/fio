/*
 * librpma_client I/O engine
 *
 * librpma_client I/O engine based on the librpma PMDK library.
 * Supports both RDMA memory semantics and channel semantics
 *   for the InfiniBand, RoCE and iWARP protocols.
 * Supports both persistent and volatile memory.
 *
 * It's a client part of the engine. See also: librpma_server
 *
 * You will need the Linux RDMA software installed
 * either from your Linux distributor or directly from openfabrics.org:
 * https://www.openfabrics.org/downloads/OFED
 *
 * You will need the librpma library installed:
 * https://github.com/pmem/rpma
 *
 * Exchanging steps of librpma_client ioengine control messages:
 *XXX
 *	1. client side sends test mode (RDMA_WRITE/RDMA_READ/SEND)
 *	   to server side.
 *	2. server side parses test mode, and sends back confirmation
 *	   to client side. In RDMA WRITE/READ test, this confirmation
 *	   includes memory information, such as rkey, address.
 *	3. client side initiates test loop.
 *	4. In RDMA WRITE/READ test, client side sends a completion
 *	   notification to server side. Server side updates its
 *	   td->done as true.
 *
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <pthread.h>
#include <inttypes.h>

#include "../fio.h"
#include "../hash.h"
#include "../optgroup.h"

#include <librpma.h>
#include <rdma/rdma_cma.h>

#define FIO_RDMA_MAX_IO_DEPTH    512
#define KILOBYTE 1024

/* XXX: to be removed (?) */
enum librpma_io_mode {
	FIO_RDMA_UNKNOWN = 0,
	FIO_RDMA_MEM_WRITE,
	FIO_RDMA_MEM_READ,
	FIO_RDMA_CHA_SEND,
	FIO_RDMA_CHA_RECV
};

struct fio_librpma_client_options {
	struct thread_data *td;
	char *server_port;
	char *server_ip;
};

static struct fio_option options[] = {
	{
		.name	= "server_ip",
		.lname	= "librpma_client engine server ip",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct fio_librpma_client_options, server_ip),
		.help	= "Server's IP to use for RDMA connections",
		.def    = "",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBRPMA,
	},
	{
		.name	= "server_port",
		.lname	= "librpma_client engine server port",
		.type	= FIO_OPT_STR_STORE,
		.off1	= offsetof(struct fio_librpma_client_options, server_port),
		.help	= "Server's port to use for RDMA connections",
		.def    = "",
		.category = FIO_OPT_C_ENGINE,
		.group	= FIO_OPT_G_LIBRPMA,
	},
	{
		.name	= NULL,
	},
};

struct remote_u {
	uint64_t buf;
	uint32_t rkey;
	uint32_t size;
};

struct librpma_info_blk {
	uint32_t mode;		/* channel semantic or memory semantic */
	uint32_t nr;		/* client: io depth
				   server: number of records for memory semantic
				 */
	uint32_t max_bs;        /* maximum block size */
	struct remote_u rmt_us[FIO_RDMA_MAX_IO_DEPTH];
};

struct librpma_io_u_data {
	uint64_t wr_id;
	struct ibv_send_wr sq_wr;
	struct ibv_recv_wr rq_wr;
	struct ibv_sge rdma_sgl;
};

/*
Note: we are thinking about creating a separate engine for the client side and
      for the server side.

- setup:
    - alloc private data (io_ops_data)

- init:
    - rpma_peer_new(ip)
    - rpma_conn_cfg_set_sq_size(iodepth + 1)
    - rpma_conn_req_new(ip, port);
    - rpma_conn_req_connect()
    - rpma_conn_get_private_data(&mr_remote)
    - rpma_mr_remote_from_descriptor()
    - rpma_mr_remote_size() >= size

- post_init - not used

- cleanup:
    - rpma_disconnect etc.
    - free private data
 */

struct librpmaio_data {
	/* required */
	struct rpma_peer *peer;
	struct rpma_conn *conn;
	struct rpma_mr_remote *mr_remote;

	struct rpma_mr_local *mr_local;

	size_t dst_offset;

	struct remote_u *rmt_us;
	int rmt_nr;

	struct frand_state rand_state;
};

#define FLUSH_ID        (void *)0xF01D
static enum fio_q_status fio_librpmaio_queue(struct thread_data *td,
					  struct io_u *io_u)
{
	struct librpmaio_data* rd = td->io_ops_data;

	fio_ro_check(td, io_u);

	/*src start point and size, right now is 0 and 1k*/
	switch (io_u->ddir) {
	case DDIR_WRITE:
		rpma_write(rd->conn, rd->mr_remote, rd->dst_offset, rd->mr_local, 0, KILOBYTE, RPMA_F_COMPLETION_ON_ERROR, NULL);
		rpma_flush(rd->conn, rd->mr_remote, rd->dst_offset, KILOBYTE,
	                        RPMA_FLUSH_TYPE_PERSISTENT, RPMA_F_COMPLETION_ALWAYS,
                                FLUSH_ID);
		break;
	}

	return	FIO_Q_COMPLETED;
}

static int fio_librpmaio_open_file(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static int fio_librpmaio_close_file(struct thread_data *td, struct fio_file *f)
{
	return 0;
}

static int fio_librpmaio_init(struct thread_data *td)
{
	struct librpmaio_data *rd = td->io_ops_data;
	struct fio_librpma_client_options *o = td->eo;
	struct ibv_context *dev = NULL;
	struct rpma_conn_req *req = NULL;
	enum rpma_conn_event conn_event = RPMA_CONN_UNDEFINED;
	struct rpma_conn_private_data pdata;
	rpma_mr_descriptor *desc;
	size_t src_size = 0;
	int ret;

	printf("fio_librpmaio_init enter, server->ip=%s!!! \n",o->server_ip);
	/* Get IBV context for the server IP */
	ret = rpma_utils_get_ibv_context(o->server_ip, RPMA_UTIL_IBV_CONTEXT_REMOTE,
			                 &dev);

	if (ret)
                return ret;
	
	printf("rpma_peer_new,dev=%p\n",dev);
	/* Create new peer */
	ret = rpma_peer_new(dev, &rd->peer);
	if (ret)
                return ret;

	/* Create a connection request */
	ret = rpma_conn_req_new(rd->peer, o->server_ip,
				o->server_port, NULL, &req);
	if (ret)
		goto err_peer_delete;

	printf("rpma_conn_req_new\n");

	/* connect the connection request and obtain the connection object */
	ret = rpma_conn_req_connect(&req, NULL, &rd->conn);
	if (ret)
		goto err_req_delete;

	/* wait for the connection to establish */
	ret = rpma_conn_next_event(rd->conn, &conn_event);
	if (ret) {
		goto err_conn_delete;
	} else if (conn_event != RPMA_CONN_ESTABLISHED) {
		goto err_conn_delete;
	}

	/* here you can use the newly established connection */
	(void) rpma_conn_get_private_data(rd->conn, &pdata);

	/*
	 * Create a remote memory registration structure from the received
	 * descriptor.
	 */
	desc = pdata.ptr;
	ret = rpma_mr_remote_from_descriptor(desc, &rd->mr_remote);
	if (ret)
		goto err_conn_disconnect;

	printf("rpma_mr_remote_from_descriptor\n");
	/* get the remote memory region size */
	ret = rpma_mr_remote_get_size(rd->mr_remote, &src_size);
	if (ret)
		goto err_mr_remote_delete;

	//rd->io_us_queued = malloc(td->o.iodepth * sizeof(struct io_u*));
	//memset(rd->io_us_queued, 0, td->o.iodepth * sizeof(struct io_u*));
	//rd->io_u_queued_nr = 0;

	//rd->io_us_flight = malloc(td->o.iodepth * sizeof(struct io_u*));
	//memset(rd->io_us_flight, 0, td->o.iodepth * sizeof(struct io_u*));
	//rd->io_u_flight_nr = 0;

	//rd->io_us_completed = malloc(td->o.iodepth * sizeof(struct io_u*));
	//memset(rd->io_us_completed, 0, td->o.iodepth * sizeof(struct io_u*));
	//rd->io_u_completed_nr = 0;
	
	return 0;

err_mr_remote_delete:
	/* delete the remote memory region's structure */
	(void) rpma_mr_remote_delete(&rd->mr_remote);
err_conn_disconnect:
	(void) rpma_conn_disconnect(rd->conn);
err_conn_delete:
	(void) rpma_conn_delete(&rd->conn);
err_req_delete:
	if (req)
		(void) rpma_conn_req_delete(&req);
err_peer_delete:
	(void) rpma_peer_delete(&rd->peer);
	
	//free(rd->io_us_queued);
	//free(rd->io_us_flight);
	//free(rd->io_us_completed);
		
	return ret;
}

static int fio_librpmaio_post_init(struct thread_data *td)
{
	return 0;
}

static void fio_librpmaio_cleanup(struct thread_data *td)
{
	struct librpmaio_data *rd = td->io_ops_data;

	if (rd)
		free(rd);
}

static int fio_librpmaio_setup(struct thread_data *td)
{
	struct librpmaio_data * rd;

	if (!td->files_index) {
		add_file(td, td->o.filename ? : "librpma_client", 0, 0);
		td->o.nr_files = td->o.nr_files ? : 1;
		td->o.open_files++;
	}

	if (!td->io_ops_data) {
		rd = malloc(sizeof(*rd));

		memset(rd, 0, sizeof(*rd));
		init_rand_seed(&rd->rand_state, (unsigned int)GOLDEN_RATIO_PRIME, 0);
		td->io_ops_data = rd;
	}

	return 0;
}

FIO_STATIC struct ioengine_ops ioengine = {
	.name			= "librpma_client",
	.version		= FIO_IOOPS_VERSION,
	.setup			= fio_librpmaio_setup,
	.init			= fio_librpmaio_init,
	.post_init		= fio_librpmaio_post_init,
	.queue			= fio_librpmaio_queue,
	//.commit			= fio_librpmaio_commit,
	//.getevents		= fio_librpmaio_getevents,
	//.event			= fio_librpmaio_event,
	.cleanup		= fio_librpmaio_cleanup,
	.open_file		= fio_librpmaio_open_file,
	.close_file		= fio_librpmaio_close_file,
	.flags			= FIO_SYNCIO| FIO_NOEXTEND, //FIO_DISKLESSIO | FIO_UNIDIR | FIO_PIPEIO,
	.options		= options,
	.option_struct_size	= sizeof(struct fio_librpma_client_options),
};

static void fio_init fio_librpma_client_register(void)
{
	register_ioengine(&ioengine);
}

static void fio_exit fio_librpma_client_unregister(void)
{
	unregister_ioengine(&ioengine);
}
