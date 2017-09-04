#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <endian.h>
#include <infiniband/verbs.h>
#include <infiniband/driver.h>

enum {
        PINGPONG_RECV_WRID = 1,
        PINGPONG_SEND_WRID = 2,
};

static int page_size;
static int use_contiguous_mr;
static int use_upstream;
static int use_odp;
static int use_ooo;	/* Mellanox HCA feature? */
static void *contig_addr;

/***
 struct pingpong_context doesn't include
 send_flag and completion_timestamp_mask member
***/
struct pingpong_context {
        struct ibv_context      *context;
        struct ibv_comp_channel *channel;
        struct ibv_pd           *pd;
        struct ibv_mr           *mr;
        struct ibv_cq           *cq;
        struct ibv_qp           *qp;
        void                    *buf;
        unsigned long long       size;
        int                      rx_depth;
        int                      pending;
        struct ibv_port_attr     portinfo;
        int                      inlr_recv;
};

struct pingpong_dest {
        int lid;
        int qpn;
        int psn;
        union ibv_gid gid;
};

static const char *guid_str(__be64 _node_guid, char *str)
{
	uint64_t node_guid = be64toh(_node_guid);
	sprintf(str, "%04x:%04x:%04x:%04x",
		(unsigned) (node_guid >> 48) & 0xffff,
		(unsigned) (node_guid >> 32) & 0xffff,
		(unsigned) (node_guid >> 16) & 0xffff,
		(unsigned) (node_guid >>  0) & 0xffff);
	return str;
}

static const char *port_state_str(enum ibv_port_state pstate)
{
	switch (pstate) {
		case IBV_PORT_DOWN:   return "PORT_DOWN";
		case IBV_PORT_INIT:   return "PORT_INIT";
		case IBV_PORT_ARMED:  return "PORT_ARMED";
		case IBV_PORT_ACTIVE: return "PORT_ACTIVE";
		default:              return "invalid state";
        }
}

static const char *port_phy_state_str(uint8_t phys_state)
{
	switch (phys_state) {
		case 1:  return "SLEEP";
		case 2:  return "POLLING";
		case 3:  return "DISABLED";
		case 4:  return "PORT_CONFIGURATION TRAINNING";
		case 5:  return "LINK_UP";
		case 6:  return "LINK_ERROR_RECOVERY";
		case 7:  return "PHY TEST";
		default: return "invalid physical state";
	}
}

static const char *transport_str(enum ibv_transport_type transport)
{
	switch (transport) {
		case IBV_TRANSPORT_IB:		return "InfiniBand";
		case IBV_TRANSPORT_IWARP:	return "iWARP";
/* some transport type doesn't support Mellanox OFED hmm...
		case IBV_TRANSPORT_USNIC:	return "usNIC";
		case IBV_TRANSPORT_USNIC_UDP:	return "usNIC UDP";
*/
		default:			return "invalid transport";
	}
}

static void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
	int i;
	uint32_t *raw = (uint32_t *)gid->raw;

	for (i = 0; i < 4; ++i)
		sprintf(&wgid[i * 8], "%08x",
			htonl(raw[i]));
}

static void wire_gid_to_gid(const char *wgid, const union ibv_gid *gid)
{
	char tmp[9];
	uint32_t v32;
	uint32_t *raw = (uint32_t *)gid->raw;
	int i;

	for (tmp[8] = 0, i = 0; i < 4; ++i) {
		memcpy(tmp, wgid + i * 8, 8);
		if (sscanf(tmp, "%x", &v32) != 1)
			v32 = 0;
		raw[i] = ntohl(v32);
	}
}

static int print_hca_info(struct ibv_device *ib_dev) {
	struct ibv_context *ctx;
	struct ibv_device_attr_ex device_attr;
	struct ibv_port_attr port_attr;
	int rc = 0;
	uint8_t port;
	char buf[256];

	ctx = ibv_open_device(ib_dev);
	if (!ctx) {
		fprintf(stderr, "Failed to open Infiniband device\n");
		rc = 1;
		goto cleanup;
        }
	if (ibv_query_device_ex(ctx, NULL, &device_attr)) {
                fprintf(stderr, "Failed to query device props\n");
                rc = 2;
		goto cleanup;
	}
	printf("\n");
	printf("Print HCA Info\n");
	printf("======================\n");
	printf("node guid:\t\t%s\n", guid_str(device_attr.orig_attr.node_guid, buf));
	printf("sys_image_guid:\t\t%s\n", guid_str(device_attr.orig_attr.sys_image_guid, buf));
	printf("phys port cnt:\t\t%d\n", device_attr.orig_attr.phys_port_cnt);
	printf("transport:\t\t%s (%d)\n",
		transport_str(ib_dev->transport_type), ib_dev->transport_type);

	for (port = 1; port <= device_attr.orig_attr.phys_port_cnt; ++port) {
		if (ibv_query_port(ctx, port, &port_attr)) {
			fprintf(stderr, "Failed to query port %u props\n", port);
			goto cleanup;
		}
		printf("state:\t\t\t%s (%d)\n",
			port_state_str(port_attr.state), port_attr.state);

		if (ib_dev->transport_type == IBV_TRANSPORT_IB)
			printf("phys_state:\t\t%s (%d)\n",
				port_phy_state_str(port_attr.phys_state), port_attr.phys_state);
	}
	printf("======================\n");
cleanup:
	if (ctx) {
		if (ibv_close_device(ctx)) {
			fprintf(stderr, "Failed to close Infiniband device\n");
			rc = 3;
		}
	}
	return rc;
}

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, unsigned long long size,
                                            int rx_depth, int port,
                                            int use_event, int inlr_recv)
{
        struct pingpong_context *ctx;
        struct ibv_exp_device_attr dattr;
        int ret;

/*	int access_flags = IBV_ACCESS_LOCAL_WRITE; */
        ctx = calloc(1, sizeof *ctx);
        if (!ctx)
                return NULL;

        memset(&dattr, 0, sizeof(dattr));

        ctx->size     = size;
        ctx->rx_depth = rx_depth;

	/***
		allocate work buffer
	***/
        if (!use_contiguous_mr) {
                ctx->buf = memalign(page_size, size);
                if (!ctx->buf) {
                        fprintf(stderr, "Couldn't allocate work buf.\n");
                        goto clean_ctx;
                }
        }

	/***
		open Infiniband device
	***/
        ctx->context = ibv_open_device(ib_dev);
        if (!ctx->context) {
                fprintf(stderr, "Couldn't get context for %s\n",
                        ibv_get_device_name(ib_dev));
                goto clean_buffer;
        }

	/***
		Initialize inline receive parameter
	***/
        if (inlr_recv) {
                dattr.comp_mask |= IBV_EXP_DEVICE_ATTR_INLINE_RECV_SZ;
                ret = ibv_exp_query_device(ctx->context, &dattr);
                if (ret) {
                        printf("  Couldn't query device for inline-receive capabilities.\n");
                } else if (!(dattr.comp_mask & IBV_EXP_DEVICE_ATTR_INLINE_RECV_SZ)) {
                        printf("  Inline-receive not supported by driver.\n");
                } else if (dattr.inline_recv_sz < inlr_recv) {
                        printf("  Max inline-receive(%d) < Requested inline-receive(%d).\n",
                               dattr.inline_recv_sz, inlr_recv);
                }
        }
	ctx->inlr_recv = inlr_recv;

	/***
		Initialize completion channel if needed
	***/
        if (use_event) {
                ctx->channel = ibv_create_comp_channel(ctx->context);
                if (!ctx->channel) {
                        fprintf(stderr, "Couldn't create completion channel\n");
                        goto clean_device;
                }
        } else
                ctx->channel = NULL;

	/***
		Initialize Protection Domain
	***/
        ctx->pd = ibv_alloc_pd(ctx->context);
        if (!ctx->pd) {
                fprintf(stderr, "Couldn't allocate PD\n");
                goto clean_comp_channel;
        }

	/***
		Register buf to memory region of the protection domain
	***/
	/* memory region is not contiguous and don't use on-demand-paging */
        if (!use_contiguous_mr && !use_odp) {
		/* simply using ibv_reg_mr to add buffer into memory-region */
                ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size,
                                     IBV_ACCESS_LOCAL_WRITE);
        } else if (use_odp) {	/* In case of using on-demand-paging */
                if (use_upstream) { /* In case of using upstream */
                        int access_flags = IBV_ACCESS_LOCAL_WRITE;
                        const uint32_t rc_caps_mask = IBV_ODP_SUPPORT_SEND |
                                              IBV_ODP_SUPPORT_RECV;
                        struct ibv_device_attr_ex attrx;
			/* get the device capabilities */
                        if (ibv_query_device_ex(ctx->context, NULL, &attrx)) {
                                fprintf(stderr, "Couldn't query device for its features\n");
                                goto clean_pd;
                        }

			/* check HCA support ODP, RC send/recv with ODP */
                        if (!(attrx.odp_caps.general_caps & IBV_ODP_SUPPORT) ||
                            (attrx.odp_caps.per_transport_caps.rc_odp_caps & rc_caps_mask) != rc_caps_mask) {
                                fprintf(stderr, "The device isn't ODP capable or does not support RC send and receive with ODP\n");
                                goto clean_pd;
                        }

			/* set the buffer flags to do on-demand-paging */
                        access_flags |= IBV_ACCESS_ON_DEMAND;
                        ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, access_flags);
                }
                else { /* In case of using un-upstream */
                        struct ibv_exp_reg_mr_in in;
                        in.pd = ctx->pd;
                        in.addr = ctx->buf;
                        in.length = size;
                        in.exp_access = IBV_EXP_ACCESS_LOCAL_WRITE | IBV_EXP_ACCESS_ON_DEMAND;
                        in.comp_mask = 0;
                        dattr.comp_mask |= IBV_EXP_DEVICE_ATTR_ODP;
                        ret = ibv_exp_query_device(ctx->context, &dattr);
                        if (ret) {
                                printf(" Couldn't query device for on-demand\
                                       paging capabilities.\n");
                                goto clean_pd;
                        } else if (!(dattr.comp_mask & IBV_EXP_DEVICE_ATTR_ODP)) {
                                printf(" On-demand paging not supported by driver.\n");
                                goto clean_pd;
                        } else if (!(dattr.odp_caps.per_transport_caps.rc_odp_caps &
                                   IBV_EXP_ODP_SUPPORT_SEND)) {
                                printf(" Send is not supported for RC transport.\n");
                                goto clean_pd;
                        } else if (!(dattr.odp_caps.per_transport_caps.rc_odp_caps &
                                   IBV_EXP_ODP_SUPPORT_RECV)) {
                                printf(" Receive is not supported for RC transport.\n");
                                goto clean_pd;
                        }

                        ctx->mr = ibv_exp_reg_mr(&in);
                }
        } else { /* In case of use_contiguous flag is set but use_odp flag isn't set */
                struct ibv_exp_reg_mr_in in;

                in.pd = ctx->pd;
                in.addr = contig_addr;
                in.length = size;
                in.exp_access = IBV_EXP_ACCESS_LOCAL_WRITE;
                if (contig_addr) {
                        in.comp_mask = IBV_EXP_REG_MR_CREATE_FLAGS;
                        in.create_flags = IBV_EXP_REG_MR_CREATE_CONTIG;
                } else {
                        in.comp_mask = 0;
                        in.exp_access |= IBV_EXP_ACCESS_ALLOCATE_MR;
                }

                ctx->mr = ibv_exp_reg_mr(&in);
        }


        if (!ctx->mr) {
                fprintf(stderr, "Couldn't register MR\n");
                goto clean_pd;
        }

	/* replace address for contiguous memory-region and sanitize work buffer */
	if (use_contiguous_mr)
		ctx->buf = ctx->mr->addr;

	/* FIXME memset(ctx->buf, 0, size); */
	memset(ctx->buf, 0x7b, size);

	/***
		Create Completion Queue
	***/
	ctx->cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
				ctx->channel, 0);
	if (!ctx->cq) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto clean_mr;
	}

	/***
		Create Queue Pair
	***/

	{
		struct ibv_exp_qp_init_attr attr = {
			.send_cq = ctx->cq,
			.recv_cq = ctx->cq,
			.cap     = {
				.max_send_wr  = 1,
				.max_recv_wr  = rx_depth,
				.max_send_sge = 1,
				.max_recv_sge = 1
			},
			.qp_type = IBV_QPT_RC,
			.pd = ctx->pd,
			.comp_mask = IBV_EXP_QP_INIT_ATTR_PD,
			.max_inl_recv = ctx->inlr_recv
		};
		if (ctx->inlr_recv)
			attr.comp_mask |= IBV_EXP_QP_INIT_ATTR_INL_RECV;

		/*** XXX Mellanox Specific? ***/
		ctx->qp = ibv_exp_create_qp(ctx->context, &attr);

		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			goto clean_cq;
		}
                if (ctx->inlr_recv > attr.max_inl_recv)
                        printf("  Actual inline-receive(%d) < requested inline-receive(%d)\n",
                               attr.max_inl_recv, ctx->inlr_recv);
        }

        {
                struct ibv_qp_attr attr = {
                        .qp_state        = IBV_QPS_INIT,
                        .pkey_index      = 0,
                        .port_num        = port,
                        .qp_access_flags = 0
                };

                if (ibv_modify_qp(ctx->qp, &attr,
                                  IBV_QP_STATE              |
                                  IBV_QP_PKEY_INDEX         |
                                  IBV_QP_PORT               |
                                  IBV_QP_ACCESS_FLAGS)) {
                        fprintf(stderr, "Failed to modify QP to INIT\n");
                        goto clean_qp;
                }
        }

        return ctx;

clean_qp:
        ibv_destroy_qp(ctx->qp);

clean_cq:
        ibv_destroy_cq(ctx->cq);

clean_mr:
        ibv_dereg_mr(ctx->mr);

clean_pd:
        ibv_dealloc_pd(ctx->pd);

clean_comp_channel:
        if (ctx->channel)
                ibv_destroy_comp_channel(ctx->channel);

clean_device:
        ibv_close_device(ctx->context);

clean_buffer:
        free(ctx->buf);

clean_ctx:
        free(ctx);

        return NULL;
}

int pp_close_ctx(struct pingpong_context *ctx)
{
        if (ibv_destroy_qp(ctx->qp)) {
                fprintf(stderr, "Couldn't destroy QP\n");
                return 1;
        }

        if (ibv_destroy_cq(ctx->cq)) {
                fprintf(stderr, "Couldn't destroy CQ\n");
                return 1;
        }

        if (ibv_dereg_mr(ctx->mr)) {
                fprintf(stderr, "Couldn't deregister MR\n");
                return 1;
        }

        if (ibv_dealloc_pd(ctx->pd)) {
                fprintf(stderr, "Couldn't deallocate PD\n");
                return 1;
        }

        if (ctx->channel) {
                if (ibv_destroy_comp_channel(ctx->channel)) {
                        fprintf(stderr, "Couldn't destroy completion channel\n");
                        return 1;
                }
        }

        if (ibv_close_device(ctx->context)) {
                fprintf(stderr, "Couldn't release context\n");
                return 1;
        }

        if (!use_contiguous_mr)
                free(ctx->buf);

        free(ctx);

        return 0;
}

/* Macros */
#define mmin(a, b) a < b ? a : b
/* SG means Scatter-Gather */
#define MAX_SGE_LEN 0xFFFFFFF

/* Add Receive Work Request n times */
static int pp_post_recv(struct pingpong_context *ctx, int n)
{
	struct ibv_sge list = {
		.addr   = (uintptr_t) ctx->buf,
		.length = mmin(ctx->size, MAX_SGE_LEN),
		.lkey   = ctx->mr->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id      = PINGPONG_RECV_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;
	int i;

	for (i = 0; i < n; ++i)
		/* Add Receive Work Request to the WR list in the Queue Pair */
		if (ibv_post_recv(ctx->qp, &wr, &bad_wr))
			break;

	return i;
}

static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct pingpong_dest *dest, int sgid_idx)
{
	/* XXX ibv_exp_qp_attr can be used by Mellanox OFED */
	/* set qp_state RTR and mtu and info about destination */
	struct ibv_exp_qp_attr attr = {
		.qp_state               = IBV_QPS_RTR,
		.path_mtu               = mtu,
		.dest_qp_num            = dest->qpn,
		.rq_psn                 = dest->psn,
		.max_dest_rd_atomic     = 1,
		.min_rnr_timer          = 12,
		.ah_attr                = {
			.is_global      = 0,
			.dlid           = dest->lid,
			.sl             = sl,
			.src_path_bits  = 0,
			.port_num       = port
		}
	};
	enum ibv_exp_qp_attr_mask attr_mask;

	/* In case of the global network */
	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_idx;
	}
	/* set attribute mask for setting attribute */
	attr_mask = IBV_QP_STATE              |
		IBV_QP_AV                 |
		IBV_QP_PATH_MTU           |
		IBV_QP_DEST_QPN           |
		IBV_QP_RQ_PSN             |
		IBV_QP_MAX_DEST_RD_ATOMIC |
		IBV_QP_MIN_RNR_TIMER;
	attr_mask |= use_ooo ? IBV_EXP_QP_OOO_RW_DATA_PLACEMENT : 0;

	/* set destination state for communicating */
	if (ibv_exp_modify_qp(ctx->qp, &attr, attr_mask)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	/* set source state for sending */
	attr.qp_state       = IBV_QPS_RTS;
	attr.timeout        = 14;
	attr.retry_cnt      = 7;
	attr.rnr_retry      = 7;
	attr.sq_psn         = my_psn;
	attr.max_rd_atomic  = 1;
	if (ibv_exp_modify_qp(ctx->qp, &attr,
			      IBV_QP_STATE              |
			      IBV_QP_TIMEOUT            |
			      IBV_QP_RETRY_CNT          |
			      IBV_QP_RNR_RETRY          |
			      IBV_QP_SQ_PSN             |
			      IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}


static struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
						 const struct pingpong_dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	/* get IP address and supported socket info of the remote node by servername */
	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		free(service);
		return NULL;
	}

	/* make a socket for connecting the remote node */
	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			/* connect to the remote node */
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}

	/* convert my_dest->gid->raw to gidw */
	gid_to_wire_gid(&my_dest->gid, gid);

	/* make connection info messages for connecting with Infiniband */
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	printf("client info msg: %s\n", msg);
	/* send client's connection info message to the server */
	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

	/* receive server's connection info message from the server */
        if (recv(sockfd, msg, sizeof(msg), MSG_WAITALL) != sizeof(msg)) {
                perror("client read");
                fprintf(stderr, "Couldn't read remote address\n");
                goto out;
        }

	/* send a completion message to the server */
        if (write(sockfd, "done", sizeof("done")) != sizeof("done")) {
                fprintf(stderr, "Couldn't send \"done\" msg\n");
                goto out;
        }

	/* gather the remote server's connection info */
        rem_dest = malloc(sizeof *rem_dest);
        if (!rem_dest)
                goto out;

        sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
                                                &rem_dest->psn, gid);
	printf("server info msg: %s\n", msg);
	/* convert gid to rem_dest->gid->raw */
        wire_gid_to_gid(gid, &rem_dest->gid);

out:
        close(sockfd);
        return rem_dest;
}

static struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
						 int ib_port, enum ibv_mtu mtu,
						 int port, int sl,
						 const struct pingpong_dest *my_dest,
						 int sgid_idx)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1, connfd;
	struct pingpong_dest *rem_dest = NULL;
	char gid[33];

	/* alloc memory and copy port num to service */
	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	/* get local HCA port's address info */
	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		free(service);
		return NULL;
	}

	/* make server socket */
	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			/* bind server socket */
			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return NULL;
	}

	listen(sockfd, 1);
	/* accept client connect request  */
	connfd = accept(sockfd, NULL, 0);
	/* close listening socket */
	close(sockfd);
	if (connfd < 0) {
		fprintf(stderr, "accept() failed\n");
		return NULL;
	}

	/* read client's msg of QP,GID,LID,PSN from socket */
	n = recv(connfd, msg, sizeof(msg), MSG_WAITALL);
	if (n != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
		goto out;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	/* read client's msg of QP,GID,LID,PSN from socket */
	sscanf(msg, "%x:%x:%x:%s", &rem_dest->lid, &rem_dest->qpn,
							&rem_dest->psn, gid);
	wire_gid_to_gid(gid, &rem_dest->gid);

	/* connect with Infiniband instead of TCP/IP */
	if (pp_connect_ctx(ctx, ib_port, my_dest->psn, mtu, sl, rem_dest,
								sgid_idx)) {
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}


	gid_to_wire_gid(&my_dest->gid, gid);
	sprintf(msg, "%04x:%06x:%06x:%s", my_dest->lid, my_dest->qpn,
							my_dest->psn, gid);
	/* send server's msg of QP,GID,LID,PSN from socket */
	if (write(connfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

	/* expecting "done" msg from client */
	if (read(connfd, msg, sizeof(msg)) <= 0) {
		fprintf(stderr, "Couldn't read \"done\" msg\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

out:
	close(connfd);
	return rem_dest;
}

static int pp_post_send(struct pingpong_context *ctx)
{
	/* set scatter gather entry list for ibv_post_send */
	struct ibv_sge list = {
		.addr   = (uintptr_t) ctx->buf,
		.length =  mmin(ctx->size, MAX_SGE_LEN),
		.lkey   = ctx->mr->lkey
	};
	/* set work request for sending by using IBV_WR_SEND */
	struct ibv_send_wr wr = {
		.wr_id      = PINGPONG_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1, /* the number of scatter gather entry */
		.opcode     = IBV_WR_SEND,/* SEND operation */
		.send_flags = IBV_SEND_SIGNALED,
                              /* send request completion event will be queued to CQ */
	};
	struct ibv_send_wr *bad_wr;

	/* set operation to HCA with mmio from work request info */
	return ibv_post_send(ctx->qp, &wr, &bad_wr);
}


/* send nop and get Work Completion from Copletion Queue and check status */
int send_nop(struct pingpong_context *ctx)
{
	struct ibv_exp_send_wr *bad_wr;
	struct ibv_exp_send_wr wr;
	struct ibv_exp_wc wc;
	int err;
	int n;

	memset(&wr, 0, sizeof(wr));

	wr.wr_id                = PINGPONG_SEND_WRID;
	wr.num_sge              = 0;
	wr.exp_opcode           = IBV_EXP_WR_NOP;
	wr.exp_send_flags       = IBV_EXP_SEND_SIGNALED;

	err = ibv_exp_post_send(ctx->qp, &wr, &bad_wr);
	if (err) {
		fprintf(stderr, "post nop failed\n");
		return err;
	}

	do {
		n = ibv_exp_poll_cq(ctx->cq, 1, &wc, sizeof(wc));
		if (n < 0) {
			fprintf(stderr, "poll CQ failed %d\n", n);
			return -1;
		}
	} while (!n);

	if (wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "completion with error %d\n", wc.status);
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	struct pingpong_context	*ctx;
	struct ibv_device	**dev_list;
	struct ibv_device	*ib_dev;
	struct pingpong_dest	my_dest;
	struct pingpong_dest	*rem_dest;
	struct timeval		start, end;
	int 			num_devices, i, ch, server_flag;
	char  			*servername = NULL;
	unsigned long long	size = 4096;
	enum ibv_mtu		mtu = IBV_MTU_1024;
	int			rx_depth = 500;
	int			iters = 1000;
	int			num_cq_events = 0;
	int			use_event = 0;
	int			inlr_recv = 0;
	int			port = 18515;
	int			ib_port = 1;
	int			qpn = 0;
	int			rcnt, scnt; /* receive/send packet counter */
	int			sl = 0;
	int			gidx = -1; /* GID table index */
	char			gid[INET6_ADDRSTRLEN];
	int			recv_wr_num; /* receive work request number */
	int			check_nop = 0;
	int			err;

	contig_addr = NULL;

	while ((ch = getopt(argc, argv, "abceu:g:i:s:r:z")) != -1) {
		switch (ch) {
		case 'a':
			check_nop = 1;
			break;
		case 'b':
			use_ooo = 1;
			break;
		case 'c':
			++use_contiguous_mr;
			break;
		case 'e':
			++use_event;
			break;
		case 'g':
			gidx = strtol(optarg, NULL, 0);
			break;
		case 'i':
			ib_port = strtol(optarg, NULL, 0);
			if (ib_port < 0) {
				fprintf(stderr, "Invalid ib_port num: \'%d\'", ib_port);
				return 1;
			}
			break;
		case 'u':
			use_upstream = 1;
			break;
		case 'r':
			rx_depth = strtol(optarg, NULL, 0);
			break;
		case 's':
			size = strtoll(optarg, NULL, 0);
			break;
		case 't':
			inlr_recv = strtol(optarg, NULL, 0);
			break;
		case 'z':
			contig_addr = (void *)(uintptr_t)strtol(optarg, NULL, 0);
			break;
		default:
			fprintf(stderr, "Invalid argument: \'%c\'", ch);
			return 1;
		}
	}

        if (optind == argc - 1)
                servername = strdupa(argv[optind]);
        else if (optind < argc) {
		fprintf(stderr, "Invalid argument");
                return 1;
        }

	if (contig_addr && !use_contiguous_mr) {
		fprintf(stderr, "Don't use both options \'z\' and \'c\' simultaneously");
		return 1;
	}

	/* Get HCA device list */
	dev_list = ibv_get_device_list(&num_devices);
	if (!dev_list) {
		perror("Failed to get Infiniband devices");
		return 1;
	}
	for (i = 0; i< num_devices; i++) {
		printf("This host has %d devices\n", num_devices);
		printf("device_name: %16s\n", ibv_get_device_name(dev_list[i]));
		printf("device guid: %16llx\n",
		       (unsigned long long) be64toh(ibv_get_device_guid(dev_list[i])));
		if (print_hca_info(dev_list[i]))
			fprintf(stderr, "Failed to do print_hca_info()\n");
	}

	ib_dev = *dev_list;

	/* Initialize PD/MR/QP/CC/CQ for pingpong */
        ctx = pp_init_ctx(ib_dev, size, rx_depth, ib_port, use_event, inlr_recv);
        if (!ctx)
                return 1;

	/* Add Receive Work Request rx_depth times */
	recv_wr_num = pp_post_recv(ctx, ctx->rx_depth);
	if (recv_wr_num < ctx->rx_depth) {
		fprintf(stderr, "Couldn't post receive (%d)\n", recv_wr_num);
		return 1;
	}

	/* register a notify entry to Completion Queue */
        if (use_event)
                if (ibv_req_notify_cq(ctx->cq, 0)) {
                        fprintf(stderr, "Couldn't request CQ notification\n");
                        return 1;
                }

	/* get LID with ibv_query_port() */
	if (ibv_query_port(ctx->context, ib_port, &ctx->portinfo)) {
		fprintf(stderr, "Couldn't get port info\n");
		return 1;
	}

	my_dest.lid = ctx->portinfo.lid;
	if (ctx->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET &&
							!my_dest.lid) {
		fprintf(stderr, "Couldn't get local LID\n");
		return 1;
	}
	printf("sm_lid:\t\t\t%d\n", ctx->portinfo.sm_lid);
	printf("port_lid:\t\t%d\n", ctx->portinfo.lid);

	/* get GID(Global Identifier) with ibv_query_gid() */
	if (gidx >= 0) {
		if (ibv_query_gid(ctx->context, ib_port, gidx, &my_dest.gid)) {
			fprintf(stderr, "can't read sgid of index %d\n", gidx);
			return 1;
		}
	} else
		memset(&my_dest.gid, 0, sizeof my_dest.gid);

	/* make my_dest entry(QPN, PSN, GID, LID) for initializing connection */
	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = lrand48() & 0xffffff; /* Packet Sequence Number */
	inet_ntop(AF_INET6, &my_dest.gid, gid, sizeof gid);
	printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
		my_dest.lid, my_dest.qpn, my_dest.psn, gid);

	if (servername)
		rem_dest = pp_client_exch_dest(servername, port, &my_dest);
	else
		rem_dest = pp_server_exch_dest(ctx, ib_port, mtu, port, sl,
								&my_dest, gidx);

	if (!rem_dest)
		return 1;

	inet_ntop(AF_INET6, &rem_dest->gid, gid, sizeof gid);
	printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
		rem_dest->lid, rem_dest->qpn, rem_dest->psn, gid);

	if (servername)
		if (pp_connect_ctx(ctx, ib_port, my_dest.psn, mtu, sl, rem_dest,
					gidx))
			return 1;

	/* XXX what pending means? */
	ctx->pending = PINGPONG_RECV_WRID;

	/* In case of client, nop and send msg */
	if (servername) {
		if (check_nop) {
			err = send_nop(ctx);
			if (err) {
				fprintf(stderr, "nop operation failed\n");
				return err;
			}
		}
		if (pp_post_send(ctx)) {
			fprintf(stderr, "Couldn't post send\n");
			return 1;
		}
		ctx->pending |= PINGPONG_SEND_WRID;
	}

	/* XXX start point must be changed */
        if (gettimeofday(&start, NULL)) {
                perror("gettimeofday");
                return 1;
        }
	/* initialize send/receive counter */
	rcnt = scnt = 0;
	while (rcnt < iters || scnt < iters) {
		if (use_event) {
			struct ibv_cq *ev_cq;
			void          *ev_ctx;

			/* wait for getting a completion event from completion channel */
			if (ibv_get_cq_event(ctx->channel, &ev_cq, &ev_ctx)) {
				fprintf(stderr, "Failed to get cq_event\n");
			        return 1;
			}

			/* the counter of completion events that have already been received */
			++num_cq_events;

			if (ev_cq != ctx->cq) {
				fprintf(stderr, "CQ event for unknown CQ %p\n", ev_cq);
				return 1;
			}

			if (ibv_req_notify_cq(ctx->cq, 0)) {
				fprintf(stderr, "Couldn't request CQ notification\n");
				return 1;
			}
		}

		{       
			struct ibv_exp_wc wc[2];
			int ne, i;

			do {    
				/* poll completion queue */
				/* now max entries of  work completion are set to 2 */
				ne = ibv_exp_poll_cq(ctx->cq, 2, wc, sizeof(wc[0]));
				if (ne < 0) {
					fprintf(stderr, "poll CQ failed %d\n", ne);
					return 1;
				}
			} while (!use_event && ne < 1);
        
			for (i = 0; i < ne; ++i) {
				/* check whether wc.status is IBV_WC_SUCCESS or not */
				if (wc[i].status != IBV_WC_SUCCESS) {
					fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
						ibv_wc_status_str(wc[i].status),
						wc[i].status, (int) wc[i].wr_id);
					return 1;
				}

				/* In this prog, wr_id is used to identify send or receive */
				switch ((int) wc[i].wr_id) {
				case PINGPONG_SEND_WRID:
					++scnt;
					break;
				case PINGPONG_RECV_WRID:
					if (--recv_wr_num <= 1) {
						/* Receive Work Request are added, if there are few recv_wr_num */
						recv_wr_num += pp_post_recv(ctx, ctx->rx_depth - recv_wr_num);
						if (recv_wr_num < ctx->rx_depth) {
							fprintf(stderr,
								"Couldn't post receive (%d)\n",
								recv_wr_num);
							return 1;
						}
					}

					++rcnt;
					break;

				default:
					fprintf(stderr, "Completion for unknown wr_id %d\n",
						(int) wc[i].wr_id);
					return 1;
				}

				ctx->pending &= ~(int) wc[i].wr_id;
				if (scnt < iters && !ctx->pending) {
					if (pp_post_send(ctx)) {
						fprintf(stderr, "Couldn't post send\n");
						return 1;
					}
					ctx->pending = PINGPONG_RECV_WRID |
						       PINGPONG_SEND_WRID;
				}
			}
		}
	}

        if (gettimeofday(&end, NULL)) {
                perror("gettimeofday");
                return 1;
        }

	{
		float usec = (end.tv_sec - start.tv_sec) * 1000000 +
			(end.tv_usec - start.tv_usec);
		long long bytes = (long long) size * iters * 2;

		printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n",
			bytes, usec / 1000000., bytes * 8. / usec);
		printf("%d iters in %.2f seconds = %.2f usec/iter\n",
			iters, usec / 1000000., usec / iters);
	}

	ibv_ack_cq_events(ctx->cq, num_cq_events);

	if (pp_close_ctx(ctx))
		return 1;

	ibv_free_device_list(dev_list);
	free(rem_dest);
	return 0;
}
