/*-
 * Copyright (c) 2017-2020, AT&T Intellectual Property.  All rights reserved.
 * Copyright (c) 2011-2016 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 */

/*
 * Master thread.
 */

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <linux/mroute.h>
#include <linux/mroute6.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <linux/if_link.h>

#ifdef HAVE_SYSTEMD
 #include <systemd/sd-daemon.h>
#endif /* HAVE_SYSTEMD */

#include <czmq.h>
#include <rte_common.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_timer.h>

#include "commands.h"
#include "compiler.h"
#include "config_internal.h"
#include "control.h"
#include "dealer.h"
#include "dp_event.h"
#include "dpmsg.h"
#include "event_internal.h"
#include "if/dpdk-eth/hotplug.h"
#include "if_ether.h"
#include "if_var.h"
#include "ip_addr.h"
#include "json_writer.h"
#include "main.h"
#include "master.h"
#include "npf/npf_event.h"
#include "route.h"
#include "route_broker.h"
#include "route_v6.h"
#include "session/session.h"
#include "shadow.h"
#include "soft_ticks.h"
#include "urcu.h"
#include "util.h"
#include "vplane_debug.h"
#include "vplane_log.h"
#include "vrf_internal.h"
#include "zmq_dp.h"

/* Frequency of updates to soft_ticks */
#define SOFT_CLOCK_HZ	    100
volatile uint64_t soft_ticks;
static uint64_t soft_clock_override;

/* How long to wait in master loop (poll).
   Determines the minimum resolution of timers used ARP, Heartbeat, etc */
#define TIMER_INTERVAL_MS    (1000/SOFT_CLOCK_HZ)

/* Min time to wait before retrying request to controller. */
#define RETRY_MIN_SEC 10
#define RETRY_MIN_TICKS (RETRY_MIN_SEC * rte_get_timer_hz())
#define RETRY_MAX_DELAY_SEC 40
#define RETRY_MAX_DELAY_TICKS (RETRY_MAX_DELAY_SEC * rte_get_timer_hz())

/* Time to wait for response to initial connection attempt */
#define CONNECT_TIMEOUT 5 /* seconds */

/* Limit for response for next part of snapshot. */
#define RESYNC_TIMEOUT 300 /* seconds */

static struct rte_timer load_average_timer;
static struct rte_timer soft_clock_timer;

struct master_time_s {
	struct rte_timer reset_timer;
	struct rte_timer connect_timer;
	struct rte_timer snapshot_timer;
	uint64_t retry_delay; /* in rte ticks */
	uint64_t connect_timeout; /* in rte ticks */
	uint64_t resync_timeout; /* in rte ticks */
};
static struct master_time_s master_time[CONT_SRC_COUNT];

/* expected asynchronous responses from controller */
struct response {
	unsigned int portid;
	uint64_t seqno;
	bool is_teardown;
	struct rte_timer timer;
	enum cont_src_en rsp_cont_src;
};
static int new_port_response(enum cont_src_en cont_src, portid_t port,
			     zmsg_t *msg, uint32_t *ifindex, char **ifname);
static zlist_t *response_list[CONT_SRC_COUNT];
static zlist_t *resync_list[CONT_SRC_COUNT];
static void master_cleanup(enum cont_src_en cont_src);

/* Uplink: Do we have an L3 source address we can use to connect to a remote
 * controller ?
 */
static bool control_addr;

enum master_state_en {
	MASTER_IDLE,
	MASTER_SETUP,
	MASTER_RESYNC_NEEDED,
	MASTER_CONNECT,
	MASTER_CONNECT_WAIT,
	MASTER_SOCKET_CREATE,
	/* The following states can handle event callbacks. They must be the
	 * highest values, and MASTER_RESYNC must be first
	 * see master_state_is_event_ready
	 */
	MASTER_RESYNC,
	MASTER_READY,
	MASTER_RESET,
};
#define MASTER_COUNT (MASTER_RESET + 1)

static const char *master_state_name(enum master_state_en state)
{
	switch (state) {
	case MASTER_IDLE:
		return "idle";
	case MASTER_SETUP:
		return "setup";
	case MASTER_RESYNC_NEEDED:
		return "resync-needed";
	case MASTER_RESYNC:
		return "resync";
	case MASTER_READY:
		return "ready";
	case MASTER_RESET:
		return "reset";
	case MASTER_CONNECT:
		return "connect";
	case MASTER_CONNECT_WAIT:
		return "connect-wait";
	case MASTER_SOCKET_CREATE:
		return "socket-create";
	default:
		return "unknown";
	}
}

struct master_state_stats_s {
	uint32_t state_in[MASTER_COUNT]; /* Times we have entered this state */
};
static struct master_state_stats_s master_state_stats[CONT_SRC_COUNT];

/*
 * Perform a dummy route lookup to the controller address to make
 * sure it's reachable via the uplink.
 */
static bool check_uplink_route(enum cont_src_en cont_src)
{
	const struct vrf *uplink_vrf;
	struct ifnet *out_ifp;

	/* VR, local vplaned and whole_dp tests all use IPC */
	if (is_local_controller() || cont_src == CONT_SRC_UPLINK ||
	    !strncmp(config.request_url, "ipc", 3))
		return true;

	uplink_vrf = get_vrf(VRF_UPLINK_ID);
	if (uplink_vrf == NULL)
		return false;

	if (config_ctrl_ip_af() == AF_INET)
		out_ifp =
			nhif_dst_lookup(uplink_vrf,
					config.remote_ip.address.ip_v4.s_addr,
					NULL);
	else
		out_ifp = nhif_dst_lookup6(uplink_vrf,
					   &config.remote_ip.address.ip_v6,
					   NULL);
	if (!out_ifp)
		return false;

	return if_is_uplink(out_ifp);
}

/* Send an event to be published by vplaned */
int dp_send_event_to_vplaned(zmsg_t *msg)
{
	zsock_t *csocket = cont_socket_get(CONT_SRC_MAIN);
	int result;

	if (!csocket)
		return -ENODEV;

	result = zmsg_pushstr(msg, "DPEVENT");
	if (result < 0)
		goto err;

	return zmsg_send_and_destroy(&msg, csocket);

 err:
	zmsg_destroy(&msg);
	return result;
}

static enum master_state_en
master_state_info(enum cont_src_en cont_src,
		  enum master_state_en new_state, bool set)
{
	static enum master_state_en master_state[CONT_SRC_COUNT]
		= { MASTER_IDLE, MASTER_IDLE };

	if (set) {
		DP_DEBUG(INIT, INFO, DATAPLANE,
			"master(%s) state change %s -> %s\n",
			cont_src_name(cont_src),
			master_state_name(master_state[cont_src]),
			master_state_name(new_state));
		master_state[cont_src] = new_state;
	}

	if ((master_state[cont_src] < MASTER_IDLE) ||
	    (master_state[cont_src] >= MASTER_COUNT))
		rte_panic("Invalid master(%s) state %i\n",
			  cont_src_name(cont_src), master_state[cont_src]);

	return master_state[cont_src];
}

static enum master_state_en
master_state_get(enum cont_src_en cont_src)
{
	return master_state_info(cont_src, 0, false);
}

static void
master_state_set(enum cont_src_en cont_src, enum master_state_en new_state)
{
	if (master_state_get(cont_src) == new_state)
		return;

	if ((cont_src == CONT_SRC_UPLINK) &&
	    (master_state_get(cont_src) == MASTER_READY))
		/* local vplaned leaving ready state, idle main */
		master_state_set(CONT_SRC_MAIN, MASTER_IDLE);

	master_state_info(cont_src, new_state, true);

	master_state_stats[cont_src].state_in[new_state]++;

	if (!is_local_controller() && (cont_src == CONT_SRC_MAIN)) {
		switch (master_state_get(cont_src)) {
		case MASTER_IDLE:
			/* remote vplaned going idle, clean up */
			master_cleanup(cont_src);
			break;
		case MASTER_READY:
			/* Reached ready state, reset the retry_delay */
			master_time[cont_src].retry_delay = RETRY_MIN_TICKS;
			break;
		default:
			break;
		}
	}
}

/* Is this source in a state that is ready to service events waiting on
 * fd / sockets
 */
static bool
master_state_is_event_ready(enum cont_src_en cont_src)
{
	return master_state_get(cont_src) >= MASTER_RESYNC;
}

static bool
master_state_all_event_ready(void)
{
	return master_state_is_event_ready(CONT_SRC_MAIN) &&
		master_state_is_event_ready(CONT_SRC_UPLINK);
}

static struct response *find_response(enum cont_src_en cont_src, uint64_t seqno)
{
	struct response *rsp;

	for (rsp = zlist_first(response_list[cont_src]);
	     rsp;
	     rsp = zlist_next(response_list[cont_src]))
		if (rsp->seqno == seqno)
			break;

	return rsp;
}

static void cleanup_responses(enum cont_src_en cont_src)
{
	struct response *rsp;

	while ((rsp = zlist_pop(response_list[cont_src])) != NULL) {
		if (rte_timer_pending(&rsp->timer))
			rte_timer_stop_sync(&rsp->timer);
		free(rsp);
	}
}

static void cleanup_resync(enum cont_src_en cont_src)
{
	zmsg_t *msg;

	while ((msg = zlist_pop(resync_list[cont_src])) != NULL)
		zmsg_destroy(&msg);
}

static void master_cleanup(enum cont_src_en cont_src)
{
	if (is_local_controller())
		return;

	console_unbind(cont_src);
	controller_unsubscribe(cont_src);
	route_broker_unsubscribe(cont_src);
	cleanup_responses(cont_src);
	cleanup_resync(cont_src);
}

/* Call back from timer every second. */
static void load_timer_event(struct rte_timer *tim __rte_unused,
			     void *arg __rte_unused)
{
	load_estimator();
}

void enable_soft_clock_override(void)
{
	soft_clock_override = 1;
}

void disable_soft_clock_override(void)
{
	soft_clock_override = 0;
}

/* Call back from soft clock timer.
   This is used to implement equivalent of jiffies in the Linux kernel.
   A value that monotonically increments periodically and is scaled
   in milliseconds. */
static void soft_clock_event(struct rte_timer *tim __rte_unused,
			     void *arg __rte_unused)
{
	if (soft_clock_override)
		return;

	soft_ticks += 1000 / SOFT_CLOCK_HZ;
}

/* Call back from timer after reset sleep has completed. */
static void reset_timer_event(struct rte_timer *tim __rte_unused,
			      void *cont_src_ptr)
{
	enum cont_src_en cont_src = (uintptr_t)cont_src_ptr;

	/*
	 * Clear the global running state.
	 *
	 * Some features do not (yet) support cleanup following
	 * a reset. Thus the simplest technique to reset the dataplane
	 * is to exit and have systemd reload the daemon.
	 */
	if (is_local_controller() || (cont_src == CONT_SRC_UPLINK)) {
		running = false;
		return;
	}

	/* Only the remote vplaned connection is being reset. */

	/* If the restart delay has got too long, restart process */
	if (master_time[cont_src].retry_delay > RETRY_MAX_DELAY_TICKS) {
		RTE_LOG(NOTICE, DATAPLANE,
			"master(%s) Shutting down, retry %lus > retry max %ds\n",
			cont_src_name(cont_src),
			master_time[cont_src].retry_delay / rte_get_timer_hz(),
			RETRY_MAX_DELAY_SEC);
		running = false;
		return;
	}

	/* Only increase retry_delay after timer expires, we may have multiple
	 * reset signals whilst timer is running.
	 */
	master_time[cont_src].retry_delay += RETRY_MIN_TICKS;

	/* Until we hear otherwise we still have an uplink with the
	 * local-vplane provided ip address.  Go back to idle state, to
	 * attempt to reconnect.
	 */
	RTE_LOG(NOTICE, DATAPLANE, "master(%s) Starting resynch\n",
		cont_src_name(cont_src));
	master_state_set(cont_src, MASTER_IDLE);
}

/* Force stop of all traffic.
   Start resynchronization process. */
void reset_dataplane(enum cont_src_en cont_src, bool delay)
{
	RTE_LOG(NOTICE, DATAPLANE,
		"master(%s) RESET, reconnecting in %lus (max %ds)\n",
		cont_src_name(cont_src),
		delay ? master_time[cont_src].retry_delay / rte_get_timer_hz()
		: 0,
		RETRY_MAX_DELAY_SEC);

	master_state_set(cont_src, MASTER_RESET);

	/* Flush old state */
	dp_event(DP_EVT_RESET_CONFIG, cont_src, NULL, 0, 0, NULL);
	lladdr_flush_all(cont_src);
	rt_flush_all(cont_src);
	rt6_flush_all(cont_src);
	if_cleanup(cont_src);

	if (delay) {
		/* Lastly set timer to delay reconnection attempt */
		rte_timer_reset(&master_time[cont_src].reset_timer,
				master_time[cont_src].retry_delay,
				SINGLE, rte_get_master_lcore(),
				reset_timer_event, (void *)cont_src);
	} else {
		/* Operator reset, return to the min retry delay. */
		master_time[cont_src].retry_delay = RETRY_MIN_TICKS;
		reset_timer_event(NULL, (void *)cont_src);
	}
}

static void handle_port_response(enum cont_src_en cont_src,
				 struct response *rsp, uint32_t ifindex,
				 char *ifname)
{
	if (ifindex) {
		struct ifnet *ifp = ifport_table[rsp->portid];

		if (ifp) {
			/* Set the if dp id to the local vplane id */
			if_set_cont_src(ifp, cont_src);
			if_rename(ifp, ifname);
			if_set_ifindex(ifp, ifindex);
			if_finish_create(ifp,
					 is_team(ifp) ? "team" : "ether",
					 NULL, &ifp->eth_addr);

			DP_DEBUG(INIT, DEBUG, DATAPLANE,
				 "master(%s) port %u ifindex %u ifname %s\n",
				 cont_src_name(cont_src), ifp->if_port,
				 ifindex, ifname);

			int rc = shadow_init_port(ifp->if_port, ifname,
						  &ifp->eth_addr);

			if (rc < 0) {
				char port_name[RTE_ETH_NAME_MAX_LEN];
				RTE_LOG(ERR, DATAPLANE,
					"master(%s) cannot init shadow for port %u\n",
					cont_src_name(cont_src), ifp->if_port);
				if (rte_eth_dev_get_name_by_port(ifp->if_port,
								 port_name) < 0)
					RTE_LOG(ERR, DATAPLANE,
						"port(%u) to name  failed\n",
						ifp->if_port);
				else if (detach_device(port_name))
					RTE_LOG(ERR, DATAPLANE,
						"detach device %s failed\n",
						port_name);
			}
		}
	}
}

static int process_ready(enum cont_src_en cont_src, zmsg_t *msg)
{
	dpmsg_t dpmsg;
	int rc;

	rc = dpmsg_convert_zmsg(msg, &dpmsg);
	if (rc >= 0)
		rc = process_ready_msg(cont_src, &dpmsg);

	zmsg_destroy(&msg);

	return rc;
}

/* Asynchronous response from server.
 * This detects when controller has restarted:
 *   LINKUP 1 127.0.0.1 -->
 *   <-- PORT FAIL
 */
static int async_response(void *cont_src_ptr)
{
	enum cont_src_en cont_src = (uintptr_t)cont_src_ptr;

	zmsg_t *msg = zmsg_recv(cont_socket_get(cont_src));

	if (!msg) {
		RTE_LOG(ERR, DATAPLANE,
		       "master(%s) no message in response from controller\n",
		       cont_src_name(cont_src));
		return -1;
	}

	if (zmsg_size(msg) < 2) {
		char *str = zmsg_popstr(msg);
		RTE_LOG(ERR, DATAPLANE,
			"master(%s) short message from controller: %s\n",
			cont_src_name(cont_src), str);
		free(str);
		goto msgerr;
	}

	/* peek at the sequence number */
	zmsg_first(msg);

	zframe_t *frame = zmsg_next(msg);
	uint64_t seqno;

	if (zframe_size(frame) != sizeof(uint64_t)) {
		RTE_LOG(ERR, DATAPLANE,
			"master(%s) expect uint64_t message got size %zd\n",
		cont_src_name(cont_src), zframe_size(frame));
		goto msgerr;
	}
	memcpy(&seqno, zframe_data(frame), sizeof(uint64_t));

	struct response *rsp = find_response(cont_src, seqno);

	if (rsp) {
		uint32_t ifindex = 0;
		char *ifname = NULL;
		if (!rsp->is_teardown) {
			int rc = new_port_response(cont_src, rsp->portid, msg,
						   &ifindex, &ifname);

			if (rc == 0)
				handle_port_response(cont_src, rsp, ifindex,
						     ifname);
			rte_timer_stop(&rsp->timer);
		}
		zlist_remove(response_list[cont_src], rsp);
		free(rsp);
		zmsg_destroy(&msg);
		free(ifname);
	} else if (master_state_get(cont_src) == MASTER_RESYNC) {
		/* stash away this message for later */
		zlist_append(resync_list[cont_src], msg);
	} else {
		/*
		 * Unsol message received in MASTER_READY
		 */
		if (process_ready(cont_src, msg) < 0) {
			RTE_LOG(ERR, DATAPLANE,
				"master(%s) unexpected message in ready\n",
				cont_src_name(cont_src));
			reset_dataplane(cont_src, true);
		}
		return 0;
	}

	/* if we have no more expected MYPORT? responses.  process whatever
	 * snapshot messages are queued.
	 */
	zmsg_t *msg2;

	while ((zlist_size(response_list[cont_src]) == 0) &&
	       (msg2 = zlist_pop(resync_list[cont_src])) != NULL) {
		int rc;
		int eof = 0;
		dpmsg_t dpmsg;

		rc = dpmsg_convert_zmsg(msg2, &dpmsg);
		if (rc < 0) {
			zmsg_destroy(&msg2);
			reset_dataplane(cont_src, true);
			break;
		}
		rc = process_snapshot_one(cont_src, &dpmsg, &eof);
		zmsg_destroy(&msg2);

		if (rc < 0) {
			reset_dataplane(cont_src, true);
			break;
		} else if (eof) {
			master_state_set(cont_src, MASTER_READY);
			controller_init_event_handler(cont_src);
			route_broker_init_event_handler(cont_src);
		}

	}
	return 0;

msgerr:
	zmsg_destroy(&msg);
	reset_dataplane(cont_src, true);
	return 0;
}

/*
 * Build and send multi-part message:
 *   [0] NEWPORT
 *   [1] <seqno> 64bit
 *   [2] <myip> ipv4/ipv6 address
 *   [3] <info> string - JSON encoded slot related info
 */
static int new_port_request(enum cont_src_en cont_src, zsock_t *zsock,
			    uint64_t seqno, const struct ifnet *ifp)
{

	zmsg_t *msg = zmsg_new();
	if (!msg)
		return -ENOMEM;

	char *devinfo = if_port_info(ifp);
	if (!devinfo) {
		zmsg_destroy(&msg);
		return -ENOMEM;
	}

	RTE_LOG(DEBUG, DATAPLANE,
		"master(%s) new port request '%s'\n", cont_src_name(cont_src),
		devinfo);

	zmsg_addstr(msg, "NEWPORT");
	zmsg_addmem(msg, &seqno, sizeof(seqno));
	zmsg_addmem(msg, &config.local_ip, sizeof(struct ip_addr));
	zmsg_addstr(msg, devinfo);
	free(devinfo);

	zmsg_send_and_destroy(&msg, zsock);
	return 0;
}

/*
 * Build and send multi-part message:
 *   [0] DELPORT
 *   [1] <seqno>  64bit
 *   [2] <port> 32bit
 *   [3] <ifindex>  32bit
 *   [4] <myip> ipv4/ipv6 address
 */
static void del_port_request(enum cont_src_en cont_src, zsock_t *zsock,
			     uint64_t seqno, const struct ifnet *ifp)
{
	uint32_t port;
	zmsg_t *msg = zmsg_new();
	if (!msg)
		return;

	zmsg_addstr(msg, "DELPORT");
	zmsg_addmem(msg, &seqno, sizeof(seqno));
	/* controller expects 32 bit value for port */
	port = ifp->if_port;
	zmsg_addmem(msg, &port, sizeof(port));
	zmsg_addmem(msg, &ifp->if_index, sizeof(ifp->if_index));
	zmsg_addmem(msg, &config.local_ip, sizeof(struct ip_addr));

	RTE_LOG(DEBUG, DATAPLANE,
		"master(%s) del port request port %u if_index %u\n",
		cont_src_name(cont_src), port, ifp->if_index);

	zmsg_send_and_destroy(&msg, zsock);
}

/*
 * Parse response from controller:
 * Expect:
 *  [0] OK
 *  [1] seqno
 *  [2] ifindex - 32bit host byte order
 *  [3] ifname - interface name
 *
 * Returns:
 *   0  - not found or protocol error
 *   <0 - error
 */
static int new_port_response(enum cont_src_en cont_src, portid_t port,
			     zmsg_t *msg, uint32_t *ifindex, char **ifname)
{
	char *answer;
	uint64_t seqno;
	int retval = -EINVAL;

	answer = zmsg_popstr(msg);
	if (!answer) {
		RTE_LOG(ERR, DATAPLANE,
			"master(%s) missing status in initial response\n",
			cont_src_name(cont_src));
		goto fail;
	}
	if (!streq(answer, "OK")) {
		RTE_LOG(ERR, DATAPLANE,
			"master(%s) got '%s' from controller\n", answer,
			cont_src_name(cont_src));
		goto fail;
	}
	if (zmsg_popu64(msg, &seqno) < 0) {
		RTE_LOG(ERR, DATAPLANE,
			"master(%s) missing seqno in response\n",
			cont_src_name(cont_src));
		goto fail;
	}
	if (zmsg_popu32(msg, ifindex) < 0) {
		RTE_LOG(ERR, DATAPLANE,
			"master(%s) missing ifindex in response\n",
			cont_src_name(cont_src));
		goto fail;
	}
	*ifname = zmsg_popstr(msg);
	if (!*ifname) {
		RTE_LOG(ERR, DATAPLANE,
			"master(%s) missing ifname in response\n",
			cont_src_name(cont_src));
		goto fail;
	}

	RTE_LOG(DEBUG, DATAPLANE,
		"master(%s) new port %u response %s(%u->%u)\n",
		cont_src_name(cont_src), port, *ifname, *ifindex,
		cont_src_ifindex(cont_src, *ifindex));
	*ifindex = cont_src_ifindex(cont_src, *ifindex);

	retval = 0;

fail:
	if (retval < 0) {
		*ifname = NULL;
		*ifindex = 0;
	}
	free(answer);
	return retval;
}

static void connect_timeout(struct rte_timer *t __unused, void *cont_src_ptr)
{
	enum cont_src_en cont_src = (uintptr_t)cont_src_ptr;

	if (master_state_get(cont_src) == MASTER_CONNECT_WAIT) {
		RTE_LOG(ERR, DATAPLANE,
			"master(%s) controller connect timeout\n",
			cont_src_name(cont_src));
		reset_dataplane(cont_src, true);
	}
}

static void snapshot_timeout(struct rte_timer *t __unused, void *cont_src_ptr)
{
	enum cont_src_en cont_src = (uintptr_t)cont_src_ptr;

	if (master_state_get(cont_src) == MASTER_RESYNC) {
		RTE_LOG(ERR, DATAPLANE,
			"master(%s) controller snapshot timeout\n",
			cont_src_name(cont_src));
		reset_dataplane(cont_src, true);
	}
}

/*
 * The controller took to long to answer.  Clean up and reset
 */
static void expire_response(struct rte_timer *t __unused, void *arg)
{
	struct response *rsp = arg;

	RTE_LOG(ERR, DATAPLANE,
		"master(%s) controller response for port %u timeout [seqno %"PRIu64"]\n",
		cont_src_name(rsp->rsp_cont_src), rsp->portid, rsp->seqno);
	reset_dataplane(rsp->rsp_cont_src, true);


}

/*
 * Initialize all the pseudo-devices (tunnels) on the controller.
 */
static int setup_interfaces(uint8_t startid, uint8_t num_ports,
			    enum cont_src_en cont_src, bool is_teardown)
{
	zsock_t *ctrl_socket = cont_socket_get(cont_src);
	unsigned int portid;
	uint64_t seqno = random();

	if ((startid + num_ports) > DATAPLANE_MAX_PORTS) {
		RTE_LOG(ERR, DATAPLANE,
			"requested portid %u out of range\n",
			(startid + num_ports));
		return -1;
	}

	for (portid = startid; portid < startid + num_ports; portid++) {
		struct ifnet *ifp = ifport_table[portid];

		if (!ifp)
			continue;

		if (!is_local_controller()) {
			if (if_port_is_uplink(portid)) {
				if (cont_src != CONT_SRC_UPLINK)
					/* vplaned-local registers uplink */
					continue;
			} else if (cont_src != CONT_SRC_MAIN)
				/* vplaned registers all but the uplink */
				continue;
		}

		/*
		 * Bonding interfaces are represented by kernel
		 * interfaces created by the control plane, and not
		 * interfaces created by the dataplane so we don't
		 * need to issue a newport request to the controller.
		 */
		if (is_team(ifp))
			continue;

		struct response *expected = malloc(sizeof(*expected));

		if (!expected) {
			RTE_LOG(NOTICE, DATAPLANE,
				"master(%s) unable to allocate response entry\n",
				cont_src_name(cont_src));
			continue;
		}

		if (is_teardown) {
			del_port_request(cont_src, ctrl_socket, ++seqno, ifp);
		} else {
			int rc = new_port_request(cont_src, ctrl_socket,
						  ++seqno, ifp);

			if (rc != 0) {
				RTE_LOG(ERR, DATAPLANE,
					"master(%s) new_port request: %s\n",
					cont_src_name(cont_src), strerror(-rc));
				free(expected);
				return -1;
			}
		}

		expected->portid = portid;
		expected->seqno = seqno;
		expected->is_teardown = is_teardown;
		expected->rsp_cont_src = cont_src;
		rte_timer_init(&expected->timer);
		if (!is_teardown)
			rte_timer_reset(&expected->timer,
					master_time[cont_src].retry_delay,
					SINGLE, rte_get_master_lcore(),
					expire_response, expected);
		zlist_append(response_list[cont_src], expected);
	}

	return 0;
}

static int setup_interface(portid_t portid, bool is_teardown)
{
	return setup_interfaces(portid, 1, CONT_SRC_MAIN, is_teardown);
}

int setup_interface_portid(portid_t portid)
{
	return setup_interface(portid, false);
}

/*
 * Uninitialize one pseudo-devices (tunnels) on the controller.
 */
int teardown_interface_portid(portid_t portid)
{
	return setup_interface(portid, true);
}

/*
 * Build and send link status message:
 *   [0] LINKUP
 *   [1] <portid> 32bit - host byte order
 *   [2] <myip> 32bits - network byte order
 *   [3] <speed> 64 bits - network speed
 *   [4] <stats> rtnl_link_stats64 - packet statistics
 *
 *   [0] LINKDOWN
 *   [1] <portid> 32bit - host byte order
 *   [2] <myip> 32bits - network byte order
 */
void send_port_status(uint32_t port_id, const struct rte_eth_link *link)
{
	struct ifnet *ifp = ifport_table[port_id];
	zsock_t *csocket = cont_socket_get(if_is_uplink(ifp) ?
					   CONT_SRC_UPLINK :
					   CONT_SRC_MAIN);

	/* If connection to controller is not up yet (ignore) */
	if (!csocket)
		return;

	/*
	 * Unlike regular ports, the link state of bonding interfaces
	 * isn't owned by the dataplane but is determined by higher
	 * levels of the system, so don't try to override it.
	 */
	if (is_team(ifp))
		return;

	zmsg_t *msg = zmsg_new();
	if (!msg) {
		RTE_LOG(ERR, DATAPLANE, "out of memory for port status msg\n");
		return;
	}

	zmsg_addstrf(msg, "LINK%s", link->link_status ? "UP" : "DOWN");
	zmsg_addmem(msg, &port_id, sizeof(uint32_t));
	zmsg_addmem(msg, &config.local_ip, sizeof(struct ip_addr));

	if (link->link_status) {
		uint64_t speed = link->link_speed;
		struct if_data stats;

		if (!if_stats(ifport_table[port_id], &stats))
			goto send;

		zmsg_addmem(msg, &speed, sizeof(speed));
		zmsg_addstr(msg, link_duplexstr(link->link_duplex));

		struct rtnl_link_stats64 rtnl_stats = {
			.rx_packets = stats.ifi_ipackets,
			.tx_packets = stats.ifi_opackets,
			.rx_bytes   = stats.ifi_ibytes,
			.tx_bytes   = stats.ifi_obytes,
			.rx_errors  = stats.ifi_ierrors,
			.tx_errors  = stats.ifi_oerrors,
			.rx_dropped = stats.ifi_idropped,
			.tx_dropped = ifi_odropped(&stats),
			.multicast  = stats.ifi_imulticast,
		};

		zmsg_addmem(msg, &rtnl_stats, sizeof(rtnl_stats));

		uint64_t advertised = get_link_modes(ifp);
		zmsg_addmem(msg, &advertised, sizeof(advertised));
	}

send:
	zmsg_send_and_destroy(&msg, csocket);
}

/*
 * Build and send message for statistics of software/virtual device
 *   [0] STATS
 *   [1] <ifname> network device name
 *   [2] <stats> rtnl_link_stats64 - packet statistics
 */
void send_if_stats(const struct ifnet *ifp,
		   const struct if_data *sw_stats)
{
	zsock_t *csocket = cont_socket_get(CONT_SRC_MAIN);

	/* if connection to controller is not up yet (ignore) */
	if (!csocket)
		return;

	zmsg_t *msg = zmsg_new();
	if (!msg)
		return;

	zmsg_addstr(msg, "STATS");
	zmsg_addstr(msg, ifp->if_name);

	struct rtnl_link_stats64 stats = {
		.rx_packets = sw_stats->ifi_ipackets,
		.tx_packets = sw_stats->ifi_opackets,
		.rx_bytes   = sw_stats->ifi_ibytes,
		.tx_bytes   = sw_stats->ifi_obytes,
		.rx_errors  = sw_stats->ifi_ierrors,
		.tx_errors  = sw_stats->ifi_oerrors,
		.rx_dropped = sw_stats->ifi_idropped,
		.tx_dropped = ifi_odropped(sw_stats),
		.multicast  = sw_stats->ifi_imulticast,
	};

	zmsg_addmem(msg, &stats, sizeof(stats));
	zmsg_send_and_destroy(&msg, csocket);
}

/* Multicast Statistics */
/* could use struct rta_mfc_stats */
void send_sg_cnt(struct sioc_sg_req *rq, vrfid_t vrf_id, uint32_t flags)
{
	zmsg_t *msg;
	zsock_t *csocket = cont_socket_get(CONT_SRC_MAIN);

	/* if connection to controller is not up yet (ignore) */
	if (!csocket)
		return;

	msg = zmsg_new();
	if (!msg)
		return;

	zmsg_addstr(msg, "MRTSTAT");
	zmsg_addmem(msg, rq, sizeof(*rq));
	zmsg_addmem(msg, &vrf_id, sizeof(vrf_id));
	zmsg_addmem(msg, &flags, sizeof(flags));
	zmsg_send_and_destroy(&msg, csocket);
}

void send_sg6_cnt(struct sioc_sg_req6 *sr, vrfid_t vrf_id, uint32_t flags)
{
	zmsg_t *msg;
	zsock_t *csocket = cont_socket_get(CONT_SRC_MAIN);

	/* if connection to controller is not up yet (ignore) */
	if (!csocket)
		return;

	msg = zmsg_new();
	if (!msg)
		return;

	zmsg_addstr(msg, "MRT6STAT");
	zmsg_addmem(msg, sr, sizeof(*sr));
	zmsg_addmem(msg, &vrf_id, sizeof(vrf_id));
	zmsg_addmem(msg, &flags, sizeof(flags));
	zmsg_send_and_destroy(&msg, csocket);
}

static void
master_init_src(enum cont_src_en cont_src)
{
	rte_timer_init(&master_time[cont_src].reset_timer);
	rte_timer_init(&master_time[cont_src].connect_timer);
	rte_timer_init(&master_time[cont_src].snapshot_timer);
	master_time[cont_src].retry_delay = RETRY_MIN_TICKS;
	master_time[cont_src].connect_timeout =
		CONNECT_TIMEOUT * rte_get_timer_hz();
	master_time[cont_src].resync_timeout =
		RESYNC_TIMEOUT * rte_get_timer_hz();

	response_list[cont_src] = zlist_new();
	if (!response_list[cont_src])
		rte_panic("%s Unable to allocate response list\n",
			  cont_src_name(cont_src));
	resync_list[cont_src] = zlist_new();
	if (!resync_list[cont_src])
		rte_panic("%s Unable to allocate resync list\n",
			  cont_src_name(cont_src));
}

static void
master_destroy_src(enum cont_src_en cont_src)
{
	if (response_list[cont_src])
		zlist_destroy(&response_list[cont_src]);
	if (resync_list[cont_src])
		zlist_destroy(&resync_list[cont_src]);
	controller_unsubscribe(cont_src);
	route_broker_unsubscribe(cont_src);
}

static void master_control_intf(struct ifnet *ifp, uint8_t family,
				const void *addr, bool add)
{
	char addr_str[INET6_ADDRSTRLEN];
	struct ip_addr ctrladdr = {
		.type = AF_UNSPEC,
		.address.ip_v4.s_addr = 0,
	};

	if (family != config.remote_ip.type)
		return;

	if (!if_is_control_channel(ifp))
		return;

	if (!addr_store(&ctrladdr, family, addr))
		return;

	if (ctrladdr.type == AF_INET6 &&
	    IN6_IS_ADDR_LINKLOCAL(&ctrladdr.address.ip_v6))
		return;

	inet_ntop(family, addr, addr_str, sizeof(addr_str));
	RTE_LOG(INFO, DATAPLANE,
		"control intf %s(%u) addr %s %s\n",
		ifp->if_name, ifp->if_index, add ? "add" : "del",
		addr_str);

	if (add) {
		if (control_addr) {
			if (!dp_addr_eq(&config.local_ip, &ctrladdr))
				RTE_LOG(ERR, DATAPLANE,
					"control inf was set. Ignoring %s\n",
					addr_str);

			return;
		}
		if (ifa_has_addr(ifp, family)) {
			addr_store(&config.local_ip, family, addr);

			if (!config.console_url_set)
				config.console_url =
					default_endpoint_dataplane();
			control_addr = true;
		} else
			RTE_LOG(ERR, DATAPLANE,
				"control inf %s not yet usable\n",
				ifp->if_name);
	} else {
		struct in6_addr v6addr = IN6ADDR_ANY_INIT;

		if (!is_addr_set(&config.local_ip))
			return;

		if (!config.console_url_set)
			free(config.console_url);

		addr_store(&config.local_ip, config.local_ip.type, &v6addr);
		config.local_ip.type = 0;

		control_addr = false;
		/* We have no control address, idle main state machine */
		master_state_set(CONT_SRC_MAIN, MASTER_IDLE);
	}
}

/* Handle a change of interface address */
static void master_addr_sig(struct ifnet *ifp, uint32_t ifindex, uint8_t family,
			    const void *addr, bool add)
{
	if (!ifp) {
		RTE_LOG(DEBUG, DATAPLANE,
			"master addr %s on unknown intf index %u\n",
			add ? "add" : "del", ifindex);
		return;
	}
	master_control_intf(ifp, family, addr, add);
}

static void master_addr_sig_add(enum cont_src_en cont_src, struct ifnet *ifp,
		uint32_t ifindex, int family, const void *addr)
{
	if (cont_src != CONT_SRC_UPLINK)
		return;

	master_addr_sig(ifp, ifindex, family, addr, true);
}

static void master_addr_sig_del(enum cont_src_en cont_src, struct ifnet *ifp,
		uint32_t ifindex, int family, const void *addr)
{
	if (cont_src != CONT_SRC_UPLINK)
		return;

	master_addr_sig(ifp, ifindex, family, addr, false);
}

static const struct dp_event_ops master_event_ops = {
	.if_addr_add = master_addr_sig_add,
	.if_addr_delete = master_addr_sig_del,
};

static void __attribute__ ((constructor)) master_event_init(void)
{
	dp_event_register(&master_event_ops);
}


/*
 * Master lcore used for console, bridge ageing timer
 * and checking link status
 */
void master_loop(void)
{
	enum cont_src_en cont_src = CONT_SRC_MAIN;

	/* Measure thread usage (1 per sec) */
	rte_timer_init(&load_average_timer);
	rte_timer_reset(&load_average_timer,
			rte_get_timer_hz(), PERIODICAL,
			rte_get_master_lcore(), load_timer_event, NULL);

	/* Soft clock */
	rte_timer_init(&soft_clock_timer);
	rte_timer_reset(&soft_clock_timer,
			rte_get_timer_hz() / SOFT_CLOCK_HZ, PERIODICAL,
			rte_get_master_lcore(), soft_clock_event, NULL);

	master_init_src(CONT_SRC_MAIN);
	if (!is_local_controller())
		master_init_src(CONT_SRC_UPLINK);

	while (running) {
		int rc;

		if (!is_local_controller()) {
			/* Toggle sources */
			if (cont_src == CONT_SRC_MAIN)
				cont_src = CONT_SRC_UPLINK;
			else
				cont_src = CONT_SRC_MAIN;
		}
		rte_timer_manage();

		rcu_quiescent_state();
		switch (master_state_get(cont_src)) {
		case MASTER_IDLE:
			if (is_local_controller() ||
				(cont_src == CONT_SRC_UPLINK))
				master_state_set(cont_src,
						 MASTER_SOCKET_CREATE);
			/* Can we start main state machine ? */
			if ((cont_src == CONT_SRC_MAIN)
				&& (master_state_get(CONT_SRC_UPLINK)
					== MASTER_READY)
				&& control_addr)
				master_state_set(CONT_SRC_MAIN,
						 MASTER_SOCKET_CREATE);
			break;

		case MASTER_SOCKET_CREATE:
			if (console_bind(cont_src) == 0)
				master_state_set(cont_src, MASTER_CONNECT);
			break;

		case MASTER_CONNECT:
			if (!check_uplink_route(cont_src))
				break;

			rc = init_controller_connection(
				cont_socket_create(cont_src), cont_src);
			if (rc < 0)
				reset_dataplane(cont_src, true);
			else {
				struct rte_timer *timer;
				uint64_t timeout;

				timer = &master_time[cont_src].connect_timer;
				timeout = master_time[cont_src].connect_timeout;

				master_state_set(cont_src, MASTER_CONNECT_WAIT);
				rte_timer_reset(timer, timeout,
						SINGLE, rte_get_master_lcore(),
						connect_timeout,
						(void *)cont_src);
			}

			rc = init_route_broker_ctrl_connection(
				route_broker_ctrl_socket_create(cont_src),
				cont_src);
			if (rc < 0)
				reset_dataplane(cont_src, true);
			break;

		case MASTER_CONNECT_WAIT:
			rc = try_controller_response(cont_socket_get(cont_src),
						     cont_src);
			if (rc < 0) {
				if (rc != -EAGAIN)
					reset_dataplane(cont_src, true);
			} else
				master_state_set(cont_src, MASTER_SETUP);
			break;

		case MASTER_SETUP:
			/* Get conf parameters */
			conf_query(cont_src);

			/* Connect to publisher */
			controller_init(cont_src);

			/* Connect shadow interfaces to controller */
			rc = setup_interfaces(0,
					      nb_ports,
					      cont_src, false);
			if (rc < 0)
				reset_dataplane(cont_src, true);
			else
				master_state_set(cont_src,
						 MASTER_RESYNC_NEEDED);
			break;

		case MASTER_RESYNC_NEEDED:
			dp_unregister_event_socket(
				zsock_resolve(
					cont_socket_get(cont_src)));
			register_event_socket_src(
					zsock_resolve(
						cont_socket_get(cont_src)),
						async_response,
						(void *)cont_src, cont_src);
			/* Get netlink state from controller */
			rc = controller_snapshot(cont_src);
			if (rc < 0) {
				reset_dataplane(cont_src, true);
				break;
			}
			master_state_set(cont_src, MASTER_RESYNC);
			rte_timer_reset(&master_time[cont_src].snapshot_timer,
					master_time[cont_src].resync_timeout,
					SINGLE, rte_get_master_lcore(),
					snapshot_timeout, (void *)cont_src);

			break;

		case MASTER_RESYNC:
		case MASTER_RESET:
		case MASTER_READY:
			if (get_next_event(cont_src, TIMER_INTERVAL_MS,
					   master_state_all_event_ready()) < 0)
				return;
			break;
		}
		if (zsys_interrupted)
			/* zmq has caught SIGTERM or SIGINT */
			running = false;
	}

	master_destroy_src(CONT_SRC_MAIN);
	if (!is_local_controller())
		master_destroy_src(CONT_SRC_UPLINK);

	RTE_LOG(NOTICE, DATAPLANE, "Shutdown started\n");

#ifdef HAVE_SYSTEMD
	sd_notify(0, "STOPPING=1");
#endif /* HAVE_SYSTEMD */
}

static int
master_state_show(FILE *f)
{
	enum cont_src_en cont_src;
	json_writer_t *wr = jsonw_new(f);

	if (wr == NULL)
		return -1;

	jsonw_name(wr, "master_state");
	jsonw_start_object(wr);
	for (cont_src = 0; cont_src < CONT_SRC_COUNT; cont_src++) {
		enum master_state_en state = master_state_get(cont_src);

		jsonw_name(wr, cont_src_name(cont_src));
		jsonw_start_object(wr);
		jsonw_int_field(wr, master_state_name(state),
				master_state_stats[cont_src].state_in[state]);
		jsonw_end_object(wr);
	}
	jsonw_end_object(wr);

	jsonw_destroy(&wr);
	return 0;
}

/* cmd "master state" */
int
cmd_master(FILE *f, int argc, char **argv)
{
	if (argc != 2) {
		fprintf(f, "Wrong number of state command arguments\n");
		return -1;
	}
	if (strcmp(argv[1], "state") == 0)
		return master_state_show(f);

	fprintf(f, "Unknown master command\n");
	return -1;
}

/* Just for whole_dp UT */
bool
dp_test_master_ready(enum cont_src_en cont_src)
{
	if (is_local_controller())
		return master_state_get(CONT_SRC_MAIN) == MASTER_READY;

	return master_state_get(cont_src) == MASTER_READY;
}
