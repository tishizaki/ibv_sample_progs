#include <stdio.h>
#include <endian.h>
#include <infiniband/verbs.h>
#include <infiniband/driver.h>

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

int main(int argc, char *argv[])
{
	struct ibv_device **dev_list;
	int num_devices, i;

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
	ibv_free_device_list(dev_list);

	return 0;
}
