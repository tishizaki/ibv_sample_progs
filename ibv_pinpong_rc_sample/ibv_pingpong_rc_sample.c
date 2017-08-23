#include <stdio.h>
#include <unistd.h>
#include <malloc.h>
#include <endian.h>
#include <infiniband/verbs.h>
#include <infiniband/driver.h>

static int page_size;
static int use_contiguous_mr;
static int use_upstream;
static int use_odp;
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
		Create Copletion Queue
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

/* XXXXXXXXXXXXX */

int main(int argc, char *argv[])
{
	struct pingpong_context *ctx;
	struct ibv_device **dev_list;
	struct ibv_device *ib_dev;
	int num_devices, i, ch, server_flag;
	char  *server_name;
	unsigned long long size = 4096;
	int rx_depth = 500;
	int use_event = 0;
	int inlr_recv = 0;
	int ib_port = 1;
	int qpn = 0;

	contig_addr = NULL;

	while ((ch = getopt(argc, argv, "ceuC:isrzS")) != -1) {
		switch (ch) {
		case 'c':
			++use_contiguous_mr;
			break;
		case 'e':
			++use_event;
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
		case 'C':
			server_flag = 0;
			break;
		case 'S':
			server_flag = 1;
			server_name = optarg;
			break;
		default:
			fprintf(stderr, "Invalid argument: \'%c\'", ch);
/*			fprintf(stderr, "Invalid argument: \'%c\' \'%c\'", ch, optarg); */
			return 1;
		}
	}

	if (contig_addr && !use_contiguous_mr) {
		fprintf(stderr, "Don't use both options \'z\' and \'c\' simultaneously");
		return 1;
	}

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
        ctx = pp_init_ctx(ib_dev, size, rx_depth, ib_port, use_event, inlr_recv);
        if (!ctx)
                return 1;
	qpn = ctx->qp->qp_num;
	printf("QP Num is 0x%06x\n", qpn);

	if (pp_close_ctx(ctx))
		return 1;

	ibv_free_device_list(dev_list);
	return 0;
}
