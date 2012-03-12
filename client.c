#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>

#include "fio.h"
#include "client.h"
#include "server.h"
#include "flist.h"
#include "hash.h"

static void handle_du(struct fio_client *client, struct fio_net_cmd *cmd);
static void handle_ts(struct fio_client *client, struct fio_net_cmd *cmd);
static void handle_gs(struct fio_client *client, struct fio_net_cmd *cmd);
static void handle_probe(struct fio_client *client, struct fio_net_cmd *cmd);
static void handle_text(struct fio_client *client, struct fio_net_cmd *cmd);
static void handle_stop(struct fio_client *client, struct fio_net_cmd *cmd);
static void handle_start(struct fio_client *client, struct fio_net_cmd *cmd);

struct client_ops fio_client_ops = {
	.text_op	= handle_text,
	.disk_util	= handle_du,
	.thread_status	= handle_ts,
	.group_stats	= handle_gs,
	.stop		= handle_stop,
	.start		= handle_start,
	.eta		= display_thread_status,
	.probe		= handle_probe,
	.eta_msec	= FIO_CLIENT_DEF_ETA_MSEC,
};

static struct timeval eta_tv;

static FLIST_HEAD(client_list);
static FLIST_HEAD(eta_list);

static FLIST_HEAD(arg_list);

struct thread_stat client_ts;
struct group_run_stats client_gs;
int sum_stat_clients;

static int sum_stat_nr;

#define FIO_CLIENT_HASH_BITS	7
#define FIO_CLIENT_HASH_SZ	(1 << FIO_CLIENT_HASH_BITS)
#define FIO_CLIENT_HASH_MASK	(FIO_CLIENT_HASH_SZ - 1)
static struct flist_head client_hash[FIO_CLIENT_HASH_SZ];

static void fio_client_add_hash(struct fio_client *client)
{
	int bucket = hash_long(client->fd, FIO_CLIENT_HASH_BITS);

	bucket &= FIO_CLIENT_HASH_MASK;
	flist_add(&client->hash_list, &client_hash[bucket]);
}

static void fio_client_remove_hash(struct fio_client *client)
{
	if (!flist_empty(&client->hash_list))
		flist_del_init(&client->hash_list);
}

static void fio_init fio_client_hash_init(void)
{
	int i;

	for (i = 0; i < FIO_CLIENT_HASH_SZ; i++)
		INIT_FLIST_HEAD(&client_hash[i]);
}

static struct fio_client *find_client_by_fd(int fd)
{
	int bucket = hash_long(fd, FIO_CLIENT_HASH_BITS) & FIO_CLIENT_HASH_MASK;
	struct fio_client *client;
	struct flist_head *entry;

	flist_for_each(entry, &client_hash[bucket]) {
		client = flist_entry(entry, struct fio_client, hash_list);

		if (client->fd == fd)
			return fio_get_client(client);
	}

	return NULL;
}

static void remove_client(struct fio_client *client)
{
	assert(client->refs);

	if (--client->refs)
		return;

	dprint(FD_NET, "client: removed <%s>\n", client->hostname);
	flist_del(&client->list);

	fio_client_remove_hash(client);

	if (!flist_empty(&client->eta_list)) {
		flist_del_init(&client->eta_list);
		fio_client_dec_jobs_eta(client->eta_in_flight, client->ops->eta);
	}

	free(client->hostname);
	if (client->argv)
		free(client->argv);
	if (client->name)
		free(client->name);

	free(client);
	nr_clients--;
	sum_stat_clients--;
}

void fio_put_client(struct fio_client *client)
{
	remove_client(client);
}

struct fio_client *fio_get_client(struct fio_client *client)
{
	client->refs++;
	return client;
}

static void __fio_client_add_cmd_option(struct fio_client *client,
					const char *opt)
{
	int index;

	index = client->argc++;
	client->argv = realloc(client->argv, sizeof(char *) * client->argc);
	client->argv[index] = strdup(opt);
	dprint(FD_NET, "client: add cmd %d: %s\n", index, opt);
}

void fio_client_add_cmd_option(void *cookie, const char *opt)
{
	struct fio_client *client = cookie;
	struct flist_head *entry;

	if (!client || !opt)
		return;

	__fio_client_add_cmd_option(client, opt);

	/*
	 * Duplicate arguments to shared client group
	 */
	flist_for_each(entry, &arg_list) {
		client = flist_entry(entry, struct fio_client, arg_list);

		__fio_client_add_cmd_option(client, opt);
	}
}

struct fio_client *fio_client_add_explicit(struct client_ops *ops,
					   const char *hostname, int type,
					   int port)
{
	struct fio_client *client;

	client = malloc(sizeof(*client));
	memset(client, 0, sizeof(*client));

	INIT_FLIST_HEAD(&client->list);
	INIT_FLIST_HEAD(&client->hash_list);
	INIT_FLIST_HEAD(&client->arg_list);
	INIT_FLIST_HEAD(&client->eta_list);
	INIT_FLIST_HEAD(&client->cmd_list);

	client->hostname = strdup(hostname);

	if (type == Fio_client_socket)
		client->is_sock = 1;
	else {
		int ipv6;

		ipv6 = type == Fio_client_ipv6;
		if (fio_server_parse_host(hostname, &ipv6,
						&client->addr.sin_addr,
						&client->addr6.sin6_addr))
			goto err;

		client->port = port;
	}

	client->fd = -1;
	client->ops = ops;
	client->refs = 1;

	__fio_client_add_cmd_option(client, "fio");

	flist_add(&client->list, &client_list);
	nr_clients++;
	dprint(FD_NET, "client: added <%s>\n", client->hostname);
	return client;
err:
	free(client);
	return NULL;
}

int fio_client_add(struct client_ops *ops, const char *hostname, void **cookie)
{
	struct fio_client *existing = *cookie;
	struct fio_client *client;

	if (existing) {
		/*
		 * We always add our "exec" name as the option, hence 1
		 * means empty.
		 */
		if (existing->argc == 1)
			flist_add_tail(&existing->arg_list, &arg_list);
		else {
			while (!flist_empty(&arg_list))
				flist_del_init(arg_list.next);
		}
	}

	client = malloc(sizeof(*client));
	memset(client, 0, sizeof(*client));

	INIT_FLIST_HEAD(&client->list);
	INIT_FLIST_HEAD(&client->hash_list);
	INIT_FLIST_HEAD(&client->arg_list);
	INIT_FLIST_HEAD(&client->eta_list);
	INIT_FLIST_HEAD(&client->cmd_list);

	if (fio_server_parse_string(hostname, &client->hostname,
					&client->is_sock, &client->port,
					&client->addr.sin_addr,
					&client->addr6.sin6_addr,
					&client->ipv6))
		return -1;

	client->fd = -1;
	client->ops = ops;
	client->refs = 1;

	__fio_client_add_cmd_option(client, "fio");

	flist_add(&client->list, &client_list);
	nr_clients++;
	dprint(FD_NET, "client: added <%s>\n", client->hostname);
	*cookie = client;
	return 0;
}

static void probe_client(struct fio_client *client)
{
	dprint(FD_NET, "client: send probe\n");

	fio_net_send_simple_cmd(client->fd, FIO_NET_CMD_PROBE, 0, &client->cmd_list);
}

static int fio_client_connect_ip(struct fio_client *client)
{
	struct sockaddr *addr;
	fio_socklen_t socklen;
	int fd, domain;

	if (client->ipv6) {
		client->addr6.sin6_family = AF_INET6;
		client->addr6.sin6_port = htons(client->port);
		domain = AF_INET6;
		addr = (struct sockaddr *) &client->addr6;
		socklen = sizeof(client->addr6);
	} else {
		client->addr.sin_family = AF_INET;
		client->addr.sin_port = htons(client->port);
		domain = AF_INET;
		addr = (struct sockaddr *) &client->addr;
		socklen = sizeof(client->addr);
	}

	fd = socket(domain, SOCK_STREAM, 0);
	if (fd < 0) {
		int ret = -errno;

		log_err("fio: socket: %s\n", strerror(errno));
		return ret;
	}

	if (connect(fd, addr, socklen) < 0) {
		int ret = -errno;

		log_err("fio: connect: %s\n", strerror(errno));
		log_err("fio: failed to connect to %s:%u\n", client->hostname,
								client->port);
		close(fd);
		return ret;
	}

	return fd;
}

static int fio_client_connect_sock(struct fio_client *client)
{
	struct sockaddr_un *addr = &client->addr_un;
	fio_socklen_t len;
	int fd;

	memset(addr, 0, sizeof(*addr));
	addr->sun_family = AF_UNIX;
	strcpy(addr->sun_path, client->hostname);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		int ret = -errno;

		log_err("fio: socket: %s\n", strerror(errno));
		return ret;
	}

	len = sizeof(addr->sun_family) + strlen(addr->sun_path) + 1;
	if (connect(fd, (struct sockaddr *) addr, len) < 0) {
		int ret = -errno;

		log_err("fio: connect; %s\n", strerror(errno));
		close(fd);
		return ret;
	}

	return fd;
}

int fio_client_connect(struct fio_client *client)
{
	int fd;

	dprint(FD_NET, "client: connect to host %s\n", client->hostname);

	if (client->is_sock)
		fd = fio_client_connect_sock(client);
	else
		fd = fio_client_connect_ip(client);

	dprint(FD_NET, "client: %s connected %d\n", client->hostname, fd);

	if (fd < 0)
		return fd;

	client->fd = fd;
	fio_client_add_hash(client);
	client->state = Client_connected;

	probe_client(client);
	return 0;
}

void fio_client_terminate(struct fio_client *client)
{
	fio_net_send_simple_cmd(client->fd, FIO_NET_CMD_QUIT, 0, NULL);
}

void fio_clients_terminate(void)
{
	struct flist_head *entry;
	struct fio_client *client;

	dprint(FD_NET, "client: terminate clients\n");

	flist_for_each(entry, &client_list) {
		client = flist_entry(entry, struct fio_client, list);
		fio_client_terminate(client);
	}
}

static void sig_int(int sig)
{
	dprint(FD_NET, "client: got signal %d\n", sig);
	fio_clients_terminate();
}

static void client_signal_handler(void)
{
	struct sigaction act;

	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;
	sigaction(SIGINT, &act, NULL);

	memset(&act, 0, sizeof(act));
	act.sa_handler = sig_int;
	act.sa_flags = SA_RESTART;
	sigaction(SIGTERM, &act, NULL);
}

static int send_client_cmd_line(struct fio_client *client)
{
	struct cmd_single_line_pdu *cslp;
	struct cmd_line_pdu *clp;
	unsigned long offset;
	unsigned int *lens;
	void *pdu;
	size_t mem;
	int i, ret;

	dprint(FD_NET, "client: send cmdline %d\n", client->argc);

	lens = malloc(client->argc * sizeof(unsigned int));

	/*
	 * Find out how much mem we need
	 */
	for (i = 0, mem = 0; i < client->argc; i++) {
		lens[i] = strlen(client->argv[i]) + 1;
		mem += lens[i];
	}

	/*
	 * We need one cmd_line_pdu, and argc number of cmd_single_line_pdu
	 */
	mem += sizeof(*clp) + (client->argc * sizeof(*cslp));

	pdu = malloc(mem);
	clp = pdu;
	offset = sizeof(*clp);

	for (i = 0; i < client->argc; i++) {
		uint16_t arg_len = lens[i];

		cslp = pdu + offset;
		strcpy((char *) cslp->text, client->argv[i]);
		cslp->len = cpu_to_le16(arg_len);
		offset += sizeof(*cslp) + arg_len;
	}

	free(lens);
	clp->lines = cpu_to_le16(client->argc);
	ret = fio_net_send_cmd(client->fd, FIO_NET_CMD_JOBLINE, pdu, mem, 0);
	free(pdu);
	return ret;
}

int fio_clients_connect(void)
{
	struct fio_client *client;
	struct flist_head *entry, *tmp;
	int ret;

#ifdef WIN32
	WSADATA wsd;
	WSAStartup(MAKEWORD(2,2), &wsd);
#endif

	dprint(FD_NET, "client: connect all\n");

	client_signal_handler();

	flist_for_each_safe(entry, tmp, &client_list) {
		client = flist_entry(entry, struct fio_client, list);

		ret = fio_client_connect(client);
		if (ret) {
			remove_client(client);
			continue;
		}

		if (client->argc > 1)
			send_client_cmd_line(client);
	}

	return !nr_clients;
}

int fio_start_client(struct fio_client *client)
{
	dprint(FD_NET, "client: start %s\n", client->hostname);
	return fio_net_send_simple_cmd(client->fd, FIO_NET_CMD_RUN, 0, NULL);
}

int fio_start_all_clients(void)
{
	struct fio_client *client;
	struct flist_head *entry, *tmp;
	int ret;

	dprint(FD_NET, "client: start all\n");

	flist_for_each_safe(entry, tmp, &client_list) {
		client = flist_entry(entry, struct fio_client, list);

		ret = fio_start_client(client);
		if (ret) {
			remove_client(client);
			continue;
		}
	}

	return flist_empty(&client_list);
}

/*
 * Send file contents to server backend. We could use sendfile(), but to remain
 * more portable lets just read/write the darn thing.
 */
static int __fio_client_send_ini(struct fio_client *client, const char *filename)
{
	struct stat sb;
	char *p, *buf;
	off_t len;
	int fd, ret;

	dprint(FD_NET, "send ini %s to %s\n", filename, client->hostname);

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		int ret = -errno;

		log_err("fio: job file <%s> open: %s\n", filename, strerror(errno));
		return ret;
	}

	if (fstat(fd, &sb) < 0) {
		int ret = -errno;

		log_err("fio: job file stat: %s\n", strerror(errno));
		close(fd);
		return ret;
	}

	buf = malloc(sb.st_size);

	len = sb.st_size;
	p = buf;
	do {
		ret = read(fd, p, len);
		if (ret > 0) {
			len -= ret;
			if (!len)
				break;
			p += ret;
			continue;
		} else if (!ret)
			break;
		else if (errno == EAGAIN || errno == EINTR)
			continue;
	} while (1);

	if (len) {
		log_err("fio: failed reading job file %s\n", filename);
		close(fd);
		free(buf);
		return 1;
	}

	client->sent_job = 1;
	ret = fio_net_send_cmd(client->fd, FIO_NET_CMD_JOB, buf, sb.st_size, 0);
	free(buf);
	close(fd);
	return ret;
}

int fio_client_send_ini(struct fio_client *client, const char *filename)
{
	int ret;

	ret = __fio_client_send_ini(client, filename);
	if (ret) {
		remove_client(client);
		return ret;
	}

	client->sent_job = 1;
	return ret;
}

int fio_clients_send_ini(const char *filename)
{
	struct fio_client *client;
	struct flist_head *entry, *tmp;

	flist_for_each_safe(entry, tmp, &client_list) {
		client = flist_entry(entry, struct fio_client, list);

		fio_client_send_ini(client, filename);
	}

	return !nr_clients;
}

static void convert_io_stat(struct io_stat *dst, struct io_stat *src)
{
	dst->max_val	= le64_to_cpu(src->max_val);
	dst->min_val	= le64_to_cpu(src->min_val);
	dst->samples	= le64_to_cpu(src->samples);

	/*
	 * Floats arrive as IEEE 754 encoded uint64_t, convert back to double
	 */
	dst->mean.u.f	= fio_uint64_to_double(le64_to_cpu(dst->mean.u.i));
	dst->S.u.f	= fio_uint64_to_double(le64_to_cpu(dst->S.u.i));
}

static void convert_ts(struct thread_stat *dst, struct thread_stat *src)
{
	int i, j;

	dst->error	= le32_to_cpu(src->error);
	dst->groupid	= le32_to_cpu(src->groupid);
	dst->pid	= le32_to_cpu(src->pid);
	dst->members	= le32_to_cpu(src->members);

	for (i = 0; i < 2; i++) {
		convert_io_stat(&dst->clat_stat[i], &src->clat_stat[i]);
		convert_io_stat(&dst->slat_stat[i], &src->slat_stat[i]);
		convert_io_stat(&dst->lat_stat[i], &src->lat_stat[i]);
		convert_io_stat(&dst->bw_stat[i], &src->bw_stat[i]);
	}

	dst->usr_time		= le64_to_cpu(src->usr_time);
	dst->sys_time		= le64_to_cpu(src->sys_time);
	dst->ctx		= le64_to_cpu(src->ctx);
	dst->minf		= le64_to_cpu(src->minf);
	dst->majf		= le64_to_cpu(src->majf);
	dst->clat_percentiles	= le64_to_cpu(src->clat_percentiles);

	for (i = 0; i < FIO_IO_U_LIST_MAX_LEN; i++) {
		fio_fp64_t *fps = &src->percentile_list[i];
		fio_fp64_t *fpd = &dst->percentile_list[i];

		fpd->u.f = fio_uint64_to_double(le64_to_cpu(fps->u.i));
	}

	for (i = 0; i < FIO_IO_U_MAP_NR; i++) {
		dst->io_u_map[i]	= le32_to_cpu(src->io_u_map[i]);
		dst->io_u_submit[i]	= le32_to_cpu(src->io_u_submit[i]);
		dst->io_u_complete[i]	= le32_to_cpu(src->io_u_complete[i]);
	}

	for (i = 0; i < FIO_IO_U_LAT_U_NR; i++) {
		dst->io_u_lat_u[i]	= le32_to_cpu(src->io_u_lat_u[i]);
		dst->io_u_lat_m[i]	= le32_to_cpu(src->io_u_lat_m[i]);
	}

	for (i = 0; i < 2; i++)
		for (j = 0; j < FIO_IO_U_PLAT_NR; j++)
			dst->io_u_plat[i][j] = le32_to_cpu(src->io_u_plat[i][j]);

	for (i = 0; i < 3; i++) {
		dst->total_io_u[i]	= le64_to_cpu(src->total_io_u[i]);
		dst->short_io_u[i]	= le64_to_cpu(src->short_io_u[i]);
	}

	dst->total_submit	= le64_to_cpu(src->total_submit);
	dst->total_complete	= le64_to_cpu(src->total_complete);

	for (i = 0; i < 2; i++) {
		dst->io_bytes[i]	= le64_to_cpu(src->io_bytes[i]);
		dst->runtime[i]		= le64_to_cpu(src->runtime[i]);
	}

	dst->total_run_time	= le64_to_cpu(src->total_run_time);
	dst->continue_on_error	= le16_to_cpu(src->continue_on_error);
	dst->total_err_count	= le64_to_cpu(src->total_err_count);
	dst->first_error	= le32_to_cpu(src->first_error);
	dst->kb_base		= le32_to_cpu(src->kb_base);
}

static void convert_gs(struct group_run_stats *dst, struct group_run_stats *src)
{
	int i;

	for (i = 0; i < 2; i++) {
		dst->max_run[i]		= le64_to_cpu(src->max_run[i]);
		dst->min_run[i]		= le64_to_cpu(src->min_run[i]);
		dst->max_bw[i]		= le64_to_cpu(src->max_bw[i]);
		dst->min_bw[i]		= le64_to_cpu(src->min_bw[i]);
		dst->io_kb[i]		= le64_to_cpu(src->io_kb[i]);
		dst->agg[i]		= le64_to_cpu(src->agg[i]);
	}

	dst->kb_base	= le32_to_cpu(src->kb_base);
	dst->groupid	= le32_to_cpu(src->groupid);
}

static void handle_ts(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct cmd_ts_pdu *p = (struct cmd_ts_pdu *) cmd->payload;

	show_thread_status(&p->ts, &p->rs);

	if (sum_stat_clients == 1)
		return;

	sum_thread_stats(&client_ts, &p->ts, sum_stat_nr);
	sum_group_stats(&client_gs, &p->rs);

	client_ts.members++;
	client_ts.groupid = p->ts.groupid;

	if (++sum_stat_nr == sum_stat_clients) {
		strcpy(client_ts.name, "All clients");
		show_thread_status(&client_ts, &client_gs);
	}
}

static void handle_gs(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct group_run_stats *gs = (struct group_run_stats *) cmd->payload;

	show_group_stats(gs);
}

static void handle_text(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct cmd_text_pdu *pdu = (struct cmd_text_pdu *) cmd->payload;
	const char *buf = (const char *) pdu->buf;
	const char *name;
	int fio_unused ret;

	name = client->name ? client->name : client->hostname;

	if (!client->skip_newline)
		fprintf(f_out, "<%s> ", name);
	ret = fwrite(buf, pdu->buf_len, 1, f_out);
	fflush(f_out);
	client->skip_newline = strchr(buf, '\n') == NULL;
}

static void convert_agg(struct disk_util_agg *agg)
{
	int i;

	for (i = 0; i < 2; i++) {
		agg->ios[i]	= le32_to_cpu(agg->ios[i]);
		agg->merges[i]	= le32_to_cpu(agg->merges[i]);
		agg->sectors[i]	= le64_to_cpu(agg->sectors[i]);
		agg->ticks[i]	= le32_to_cpu(agg->ticks[i]);
	}

	agg->io_ticks		= le32_to_cpu(agg->io_ticks);
	agg->time_in_queue	= le32_to_cpu(agg->time_in_queue);
	agg->slavecount		= le32_to_cpu(agg->slavecount);
	agg->max_util.u.f	= fio_uint64_to_double(__le64_to_cpu(agg->max_util.u.i));
}

static void convert_dus(struct disk_util_stat *dus)
{
	int i;

	for (i = 0; i < 2; i++) {
		dus->ios[i]	= le32_to_cpu(dus->ios[i]);
		dus->merges[i]	= le32_to_cpu(dus->merges[i]);
		dus->sectors[i]	= le64_to_cpu(dus->sectors[i]);
		dus->ticks[i]	= le32_to_cpu(dus->ticks[i]);
	}

	dus->io_ticks		= le32_to_cpu(dus->io_ticks);
	dus->time_in_queue	= le32_to_cpu(dus->time_in_queue);
	dus->msec		= le64_to_cpu(dus->msec);
}

static void handle_du(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct cmd_du_pdu *du = (struct cmd_du_pdu *) cmd->payload;

	if (!client->disk_stats_shown) {
		client->disk_stats_shown = 1;
		log_info("\nDisk stats (read/write):\n");
	}

	print_disk_util(&du->dus, &du->agg, terse_output);
}

static void convert_jobs_eta(struct jobs_eta *je)
{
	int i;

	je->nr_running		= le32_to_cpu(je->nr_running);
	je->nr_ramp		= le32_to_cpu(je->nr_ramp);
	je->nr_pending		= le32_to_cpu(je->nr_pending);
	je->files_open		= le32_to_cpu(je->files_open);

	for (i = 0; i < 2; i++) {
		je->m_rate[i]		= le32_to_cpu(je->m_rate[i]);
		je->t_rate[i]		= le32_to_cpu(je->t_rate[i]);
		je->m_iops[i]		= le32_to_cpu(je->m_iops[i]);
		je->t_iops[i]		= le32_to_cpu(je->t_iops[i]);
		je->rate[i]	= le32_to_cpu(je->rate[i]);
		je->iops[i]	= le32_to_cpu(je->iops[i]);
	}

	je->elapsed_sec		= le64_to_cpu(je->elapsed_sec);
	je->eta_sec		= le64_to_cpu(je->eta_sec);
	je->nr_threads		= le32_to_cpu(je->nr_threads);
}

void fio_client_sum_jobs_eta(struct jobs_eta *dst, struct jobs_eta *je)
{
	int i;

	dst->nr_running		+= je->nr_running;
	dst->nr_ramp		+= je->nr_ramp;
	dst->nr_pending		+= je->nr_pending;
	dst->files_open		+= je->files_open;

	for (i = 0; i < 2; i++) {
		dst->m_rate[i]	+= je->m_rate[i];
		dst->t_rate[i]	+= je->t_rate[i];
		dst->m_iops[i]	+= je->m_iops[i];
		dst->t_iops[i]	+= je->t_iops[i];
		dst->rate[i]	+= je->rate[i];
		dst->iops[i]	+= je->iops[i];
	}

	dst->elapsed_sec	+= je->elapsed_sec;

	if (je->eta_sec > dst->eta_sec)
		dst->eta_sec = je->eta_sec;

	dst->nr_threads		+= je->nr_threads;
	/* we need to handle je->run_str too ... */
}

void fio_client_dec_jobs_eta(struct client_eta *eta, client_eta_op eta_fn)
{
	if (!--eta->pending) {
		eta_fn(&eta->eta);
		free(eta);
	}
}

static void remove_reply_cmd(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct fio_net_int_cmd *icmd = NULL;
	struct flist_head *entry;

	flist_for_each(entry, &client->cmd_list) {
		icmd = flist_entry(entry, struct fio_net_int_cmd, list);

		if (cmd->tag == (uintptr_t) icmd)
			break;

		icmd = NULL;
	}

	if (!icmd) {
		log_err("fio: client: unable to find matching tag\n");
		return;
	}

	flist_del(&icmd->list);
	cmd->tag = icmd->saved_tag;
	free(icmd);
}

static void handle_eta(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct jobs_eta *je = (struct jobs_eta *) cmd->payload;
	struct client_eta *eta = (struct client_eta *) (uintptr_t) cmd->tag;

	dprint(FD_NET, "client: got eta tag %p, %d\n", eta, eta->pending);

	assert(client->eta_in_flight == eta);

	client->eta_in_flight = NULL;
	flist_del_init(&client->eta_list);

	if (client->ops->jobs_eta)
		client->ops->jobs_eta(client, je);

	fio_client_sum_jobs_eta(&eta->eta, je);
	fio_client_dec_jobs_eta(eta, client->ops->eta);
}

static void handle_probe(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct cmd_probe_pdu *probe = (struct cmd_probe_pdu *) cmd->payload;
	const char *os, *arch;
	char bit[16];

	os = fio_get_os_string(probe->os);
	if (!os)
		os = "unknown";

	arch = fio_get_arch_string(probe->arch);
	if (!arch)
		os = "unknown";

	sprintf(bit, "%d-bit", probe->bpp * 8);

	log_info("hostname=%s, be=%u, %s, os=%s, arch=%s, fio=%u.%u.%u\n",
		probe->hostname, probe->bigendian, bit, os, arch,
		probe->fio_major, probe->fio_minor, probe->fio_patch);

	if (!client->name)
		client->name = strdup((char *) probe->hostname);
}

static void handle_start(struct fio_client *client, struct fio_net_cmd *cmd)
{
	struct cmd_start_pdu *pdu = (struct cmd_start_pdu *) cmd->payload;

	client->state = Client_started;
	client->jobs = pdu->jobs;
}

static void handle_stop(struct fio_client *client, struct fio_net_cmd *cmd)
{
	if (client->error)
		log_info("client <%s>: exited with error %d\n", client->hostname, client->error);
}

static void convert_stop(struct fio_net_cmd *cmd)
{
	struct cmd_end_pdu *pdu = (struct cmd_end_pdu *) cmd->payload;

	pdu->error = le32_to_cpu(pdu->error);
}

static void convert_text(struct fio_net_cmd *cmd)
{
	struct cmd_text_pdu *pdu = (struct cmd_text_pdu *) cmd->payload;

	pdu->level	= le32_to_cpu(pdu->level);
	pdu->buf_len	= le32_to_cpu(pdu->buf_len);
	pdu->log_sec	= le64_to_cpu(pdu->log_sec);
	pdu->log_usec	= le64_to_cpu(pdu->log_usec);
}

int fio_handle_client(struct fio_client *client)
{
	struct client_ops *ops = client->ops;
	struct fio_net_cmd *cmd;

	dprint(FD_NET, "client: handle %s\n", client->hostname);

	cmd = fio_net_recv_cmd(client->fd);
	if (!cmd)
		return 0;

	dprint(FD_NET, "client: got cmd op %s from %s (pdu=%u)\n",
		fio_server_op(cmd->opcode), client->hostname, cmd->pdu_len);

	switch (cmd->opcode) {
	case FIO_NET_CMD_QUIT:
		if (ops->quit)
			ops->quit(client);
		remove_client(client);
		free(cmd);
		break;
	case FIO_NET_CMD_TEXT:
		convert_text(cmd);
		ops->text_op(client, cmd);
		free(cmd);
		break;
	case FIO_NET_CMD_DU: {
		struct cmd_du_pdu *du = (struct cmd_du_pdu *) cmd->payload;

		convert_dus(&du->dus);
		convert_agg(&du->agg);

		ops->disk_util(client, cmd);
		free(cmd);
		break;
		}
	case FIO_NET_CMD_TS: {
		struct cmd_ts_pdu *p = (struct cmd_ts_pdu *) cmd->payload;

		convert_ts(&p->ts, &p->ts);
		convert_gs(&p->rs, &p->rs);

		ops->thread_status(client, cmd);
		free(cmd);
		break;
		}
	case FIO_NET_CMD_GS: {
		struct group_run_stats *gs = (struct group_run_stats *) cmd->payload;

		convert_gs(gs, gs);

		ops->group_stats(client, cmd);
		free(cmd);
		break;
		}
	case FIO_NET_CMD_ETA: {
		struct jobs_eta *je = (struct jobs_eta *) cmd->payload;

		remove_reply_cmd(client, cmd);
		convert_jobs_eta(je);
		handle_eta(client, cmd);
		free(cmd);
		break;
		}
	case FIO_NET_CMD_PROBE:
		remove_reply_cmd(client, cmd);
		ops->probe(client, cmd);
		free(cmd);
		break;
	case FIO_NET_CMD_SERVER_START:
		client->state = Client_running;
		if (ops->job_start)
			ops->job_start(client, cmd);
		free(cmd);
		break;
	case FIO_NET_CMD_START: {
		struct cmd_start_pdu *pdu = (struct cmd_start_pdu *) cmd->payload;

		pdu->jobs = le32_to_cpu(pdu->jobs);
		ops->start(client, cmd);
		free(cmd);
		break;
		}
	case FIO_NET_CMD_STOP: {
		struct cmd_end_pdu *pdu = (struct cmd_end_pdu *) cmd->payload;

		convert_stop(cmd);
		client->state = Client_stopped;
		client->error = pdu->error;
		ops->stop(client, cmd);
		free(cmd);
		break;
		}
	case FIO_NET_CMD_ADD_JOB:
		if (ops->add_job)
			ops->add_job(client, cmd);
		free(cmd);
		break;
	default:
		log_err("fio: unknown client op: %s\n", fio_server_op(cmd->opcode));
		free(cmd);
		break;
	}

	return 1;
}

static void request_client_etas(struct client_ops *ops)
{
	struct fio_client *client;
	struct flist_head *entry;
	struct client_eta *eta;
	int skipped = 0;

	dprint(FD_NET, "client: request eta (%d)\n", nr_clients);

	eta = malloc(sizeof(*eta));
	memset(&eta->eta, 0, sizeof(eta->eta));
	eta->pending = nr_clients;

	flist_for_each(entry, &client_list) {
		client = flist_entry(entry, struct fio_client, list);

		if (!flist_empty(&client->eta_list)) {
			skipped++;
			continue;
		}
		if (client->state != Client_running)
			continue;

		assert(!client->eta_in_flight);
		flist_add_tail(&client->eta_list, &eta_list);
		client->eta_in_flight = eta;
		fio_net_send_simple_cmd(client->fd, FIO_NET_CMD_SEND_ETA,
					(uintptr_t) eta, &client->cmd_list);
	}

	while (skipped--)
		fio_client_dec_jobs_eta(eta, ops->eta);

	dprint(FD_NET, "client: requested eta tag %p\n", eta);
}

static int client_check_cmd_timeout(struct fio_client *client,
				    struct timeval *now)
{
	struct fio_net_int_cmd *cmd;
	struct flist_head *entry, *tmp;
	int ret = 0;

	flist_for_each_safe(entry, tmp, &client->cmd_list) {
		cmd = flist_entry(entry, struct fio_net_int_cmd, list);

		if (mtime_since(&cmd->tv, now) < FIO_NET_CLIENT_TIMEOUT)
			continue;

		log_err("fio: client %s, timeout on cmd %s\n", client->hostname,
						fio_server_op(cmd->cmd.opcode));
		flist_del(&cmd->list);
		free(cmd);
		ret = 1;
	}

	return flist_empty(&client->cmd_list) && ret;
}

static int fio_check_clients_timed_out(void)
{
	struct fio_client *client;
	struct flist_head *entry, *tmp;
	struct timeval tv;
	int ret = 0;

	gettimeofday(&tv, NULL);

	flist_for_each_safe(entry, tmp, &client_list) {
		client = flist_entry(entry, struct fio_client, list);

		if (flist_empty(&client->cmd_list))
			continue;

		if (!client_check_cmd_timeout(client, &tv))
			continue;

		if (client->ops->timed_out)
			client->ops->timed_out(client);
		else
			log_err("fio: client %s timed out\n", client->hostname);

		remove_client(client);
		ret = 1;
	}

	return ret;
}

int fio_handle_clients(struct client_ops *ops)
{
	struct pollfd *pfds;
	int i, ret = 0, retval = 0;

	gettimeofday(&eta_tv, NULL);

	pfds = malloc(nr_clients * sizeof(struct pollfd));

	sum_stat_clients = nr_clients;
	init_thread_stat(&client_ts);
	init_group_run_stat(&client_gs);

	while (!exit_backend && nr_clients) {
		struct flist_head *entry, *tmp;
		struct fio_client *client;

		i = 0;
		flist_for_each_safe(entry, tmp, &client_list) {
			client = flist_entry(entry, struct fio_client, list);

			if (!client->sent_job && !client->ops->stay_connected &&
			    flist_empty(&client->cmd_list)) {
				remove_client(client);
				continue;
			}

			pfds[i].fd = client->fd;
			pfds[i].events = POLLIN;
			i++;
		}

		if (!nr_clients)
			break;

		assert(i == nr_clients);

		do {
			struct timeval tv;

			gettimeofday(&tv, NULL);
			if (mtime_since(&eta_tv, &tv) >= ops->eta_msec) {
				request_client_etas(ops);
				memcpy(&eta_tv, &tv, sizeof(tv));

				if (fio_check_clients_timed_out())
					break;
			}

			ret = poll(pfds, nr_clients, 100);
			if (ret < 0) {
				if (errno == EINTR)
					continue;
				log_err("fio: poll clients: %s\n", strerror(errno));
				break;
			} else if (!ret)
				continue;
		} while (ret <= 0);

		for (i = 0; i < nr_clients; i++) {
			if (!(pfds[i].revents & POLLIN))
				continue;

			client = find_client_by_fd(pfds[i].fd);
			if (!client) {
				log_err("fio: unknown client fd %d\n", pfds[i].fd);
				continue;
			}
			if (!fio_handle_client(client)) {
				log_info("client: host=%s disconnected\n",
						client->hostname);
				remove_client(client);
				retval = 1;
			} else if (client->error)
				retval = 1;
			fio_put_client(client);
		}
	}

	free(pfds);
	return retval;
}
