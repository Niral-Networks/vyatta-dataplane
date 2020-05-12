/*
 * Copyright (c) 2018-2020, AT&T Intellectual Property. All rights reserved.
 * Copyright (c) 2015 by Brocade Communications Systems, Inc.
 * All rights reserved.
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 * dataplane UT Firewall tests for packet to/from the kernel.  Local packets
 * and packets that have been forwarded in the kernel.
 */

#include <libmnl/libmnl.h>

#include "ip6_funcs.h"
#include "ip_funcs.h"
#include "in_cksum.h"
#include "if_var.h"
#include "main.h"

#include "dp_test.h"
#include "dp_test_str.h"
#include "dp_test_lib_internal.h"
#include "dp_test_lib_exp.h"
#include "dp_test_lib_intf_internal.h"
#include "dp_test_lib_pkt.h"
#include "dp_test_pktmbuf_lib_internal.h"
#include "dp_test_netlink_state_internal.h"
#include "dp_test_console.h"
#include "dp_test_json_utils.h"
#include "dp_test_npf_lib.h"
#include "dp_test_npf_fw_lib.h"
#include "dp_test_npf_sess_lib.h"


DP_DECL_TEST_SUITE(npf_local);

DP_DECL_TEST_CASE(npf_local, ipv4, NULL, NULL);

/*
 * This test currently doesnt do any npf tests.  It simply exercises the spath
 * in/out test code.
 */
DP_START_TEST(ipv4, spath)
{
	struct dp_test_pkt_desc_t *pkt;
	struct dp_test_expected *test_exp;
	struct rte_mbuf *test_pak;

	/* Setup interfaces and neighbours */
	dp_test_nl_add_ip_addr_and_connected("dp1T0", "1.1.1.1/24");
	dp_test_netlink_add_neigh("dp1T0", "1.1.1.2",
				  "aa:bb:cc:dd:1:a1");

	/*
	 * First simulate pkt from kernel to be tx on intf1
	 */
	struct dp_test_pkt_desc_t v4_pktA = {
		.text       = "Packet A, Local -> Neighbour 1",
		.len        = 20,
		.ether_type = RTE_ETHER_TYPE_IPV4,
		.l3_src     = "1.1.1.1",
		.l2_src     = "0:0:0:0:0:0",
		.l3_dst     = "1.1.1.2",
		.l2_dst     = "aa:bb:cc:dd:1:a1",
		.proto      = IPPROTO_TCP,
		.l4         = {
			.tcp = {
				.sport = 41000,
				.dport = 1000,
				.flags = 0
			}
		},
		.rx_intf    = "dp1T0",
		.tx_intf    = "dp1T0"
	};
	pkt = &v4_pktA;

	test_pak = dp_test_from_spath_v4_pkt_from_desc(pkt);

	test_exp = dp_test_exp_create(test_pak);
	dp_test_exp_set_oif_name(test_exp, pkt->tx_intf);
	dp_test_exp_set_fwd_status(test_exp, DP_TEST_FWD_FORWARDED);

	/* Run the test.  kernel -> intf1 -> n1 */
	dp_test_send_slowpath_pkt(test_pak, test_exp);

	/*
	 * Next simulate pkt rcvd on intf1 addressed to the router.
	 */
	struct dp_test_pkt_desc_t v4_pktB = {
		.text       = "Packet B, Neighbour 1 -> Local",
		.len        = 20,
		.ether_type = RTE_ETHER_TYPE_IPV4,
		.l3_src     = "1.1.1.2",
		.l2_src     = "aa:bb:cc:dd:1:a1",
		.l3_dst     = "1.1.1.1",
		.l2_dst     = "0:0:0:0:0:0",
		.proto      = IPPROTO_TCP,
		.l4         = {
			.tcp = {
				.sport = 1000,
				.dport = 41000,
				.flags = 0
			}
		},
		.rx_intf    = "dp1T0",
		.tx_intf    = "dp1T0"
	};
	pkt = &v4_pktB;

	test_pak = dp_test_v4_pkt_from_desc(pkt);

	test_exp = dp_test_exp_create(test_pak);
	dp_test_exp_set_oif_name(test_exp, pkt->tx_intf);
	dp_test_exp_set_fwd_status(test_exp, DP_TEST_FWD_LOCAL);

	/* Run the test.  n1 -> intf1 -> kernel */
	dp_test_pak_receive(test_pak, pkt->rx_intf, test_exp);

	/* Cleanup */
	dp_test_nl_del_ip_addr_and_connected("dp1T0", "1.1.1.1/24");
	dp_test_netlink_del_neigh("dp1T0", "1.1.1.2",
				  "aa:bb:cc:dd:1:a1");

} DP_END_TEST;

/*
 * Test that a packet forwarded by the kernel passes though the output
 * interface firewall, and not the local firewall.
 */
DP_START_TEST(ipv4, kernel_forwarded)
{
	struct dp_test_pkt_desc_t *pkt;
	struct dp_test_expected *test_exp;
	struct rte_mbuf *test_pak;

	/* Setup interfaces and neighbours */
	dp_test_nl_add_ip_addr_and_connected("dp1T0", "1.1.1.1/24");
	dp_test_netlink_add_neigh("dp1T0", "1.1.1.2",
				  "aa:bb:cc:dd:1:a1");

	struct dp_test_npf_rule_t rules[] = {
		RULE_10_PASS_TO_ANY,
		RULE_DEF_BLOCK,
		NULL_RULE };

	struct dp_test_npf_ruleset_t fw = {
		.rstype = "fw-out",
		.name   = "FW1_OUT",
		.enable = 1,
		.attach_point   = "dp1T0",
		.fwd    = FWD,
		.dir    = "out",
		.rules  = rules
	};
	dp_test_npf_fw_add(&fw, false);

	/*
	 * source address is *not* an address belonging to the router.  This
	 * simulates a packet that has been forwarded by the kernel.
	 */
	struct dp_test_pkt_desc_t v4_pktA = {
		.text       = "Packet B, Non-local -> Neighbour 1",
		.len        = 20,
		.ether_type = RTE_ETHER_TYPE_IPV4,
		.l3_src     = "2.2.2.2",
		.l2_src     = "aa:bb:cc:dd:2:a2",
		.l3_dst     = "1.1.1.2",
		.l2_dst     = "aa:bb:cc:dd:1:a1",
		.proto      = IPPROTO_TCP,
		.l4         = {
			.tcp = {
				.sport = 41000,
				.dport = 1000,
				.flags = 0
			}
		},
		.rx_intf    = "dp1T0",
		.tx_intf    = "dp1T0"
	};
	pkt = &v4_pktA;

	test_pak = dp_test_from_spath_v4_pkt_from_desc(pkt);

	test_exp = dp_test_exp_create(test_pak);
	dp_test_exp_set_oif_name(test_exp, pkt->tx_intf);
	dp_test_exp_set_fwd_status(test_exp, DP_TEST_FWD_FORWARDED);

	/* Run the test.  kernel -> intf1 -> n1 */
	dp_test_send_slowpath_pkt(test_pak, test_exp);

	/* Verify firewall packet count */
	dp_test_npf_verify_rule_pkt_count(NULL, &fw, fw.rules[0].rule, 1);

	/* Cleanup */
	dp_test_npf_fw_del(&fw, false);

	dp_test_nl_del_ip_addr_and_connected("dp1T0", "1.1.1.1/24");
	dp_test_netlink_del_neigh("dp1T0", "1.1.1.2",
				  "aa:bb:cc:dd:1:a1");

} DP_END_TEST;

/*
 * Test creates ipv4 tcp packet and send it to shadow interface.
 * Originate firewall is configured in the interface to verify
 * dscp mark function and action drop.
 *
 *                                  |
 *                                  |
 *                                  v
 *                          +-----+ 1.1.1.1
 *                          |     |
 *                          | uut |---------------host 1.1.1.2
 *                          |     | dp1T0
 *                          +-----+ intf1
 *
 *              --> Forwards (on output)
 *              Source 1.1.1.1 Destination 1.1.1.2
 *
 */
DP_DECL_TEST_SUITE(npf_orig);

DP_DECL_TEST_CASE(npf_orig, ipv4_tcp_shadow, NULL, NULL);

static void npf_orig_ipv4_tcp_shadow_setup(
		struct dp_test_expected **test_exp,
		struct rte_mbuf **test_pak)
{
	struct dp_test_pkt_desc_t *pkt;
	struct rte_mbuf *exp_pak;
	struct iphdr *ip;

	/* Setup interfaces and neighbours */
	dp_test_nl_add_ip_addr_and_connected("dp1T0", "1.1.1.1/24");
	dp_test_netlink_add_neigh("dp1T0", "1.1.1.2",
				  "aa:bb:cc:dd:1:a1");

	/*
	 * Simulate pkt from kernel to be tx on intf1
	 */
	struct dp_test_pkt_desc_t v4_pktA = {
		.text       = "Packet A, Local -> Neighbour 1",
		.len        = 20,
		.ether_type = RTE_ETHER_TYPE_IPV4,
		.l3_src     = "1.1.1.1",
		.l2_src     = "0:0:0:0:0:0",
		.l3_dst     = "1.1.1.2",
		.l2_dst     = "aa:bb:cc:dd:1:a1",
		.proto      = IPPROTO_TCP,
		.l4         = {
			.tcp = {
				.sport = 41000,
				.dport = 1000,
				.flags = 0
			}
		},
		.rx_intf    = "dp1T0",
		.tx_intf    = "dp1T0"
	};
	pkt = &v4_pktA;

	*test_pak = dp_test_from_spath_v4_pkt_from_desc(pkt);

	*test_exp = dp_test_exp_create(*test_pak);
	exp_pak = dp_test_exp_get_pak_m(*test_exp, 0);
	ip = iphdr(exp_pak);
	dp_test_set_pak_ip_field(ip, DP_TEST_SET_TOS, IPTOS_DSCP_AF12);
	dp_test_exp_set_dont_care(*test_exp, 0, (uint8_t *)&ip->check, 2);
}

DP_START_TEST(ipv4_tcp_shadow, accpet_and_dscp_remark)
{
	struct dp_test_expected *test_exp = NULL;
	struct rte_mbuf *test_pak = NULL;

	struct dp_test_npf_rule_t rules[] = {
		{
			.rule     = "1",
			.pass     = PASS,
			.stateful = STATELESS,
			.npf      = "proto-final=6 src-port=41000"
						" rproc=markdscp(12)"},
		RULE_DEF_BLOCK,
		NULL_RULE };

	struct dp_test_npf_ruleset_t fw = {
		.rstype = "originate",
		.name   = "FW_TCP_ORIG",
		.enable = 1,
		.attach_point   = "dp1T0",
		.fwd    = FWD,
		.dir    = "out",
		.rules  = rules
	};
	dp_test_npf_fw_add(&fw, false);

	npf_orig_ipv4_tcp_shadow_setup(&test_exp, &test_pak);

	dp_test_exp_set_oif_name(test_exp, "dp1T0");
	dp_test_exp_set_fwd_status(test_exp, DP_TEST_FWD_FORWARDED);

	/* Run the test.  kernel -> intf1 -> n1 */
	dp_test_send_slowpath_pkt(test_pak, test_exp);

	/* Verify firewall packet count */
	dp_test_npf_verify_rule_pkt_count(NULL, &fw, fw.rules[0].rule, 1);

	/* Cleanup */
	dp_test_npf_fw_del(&fw, false);

	dp_test_nl_del_ip_addr_and_connected("dp1T0", "1.1.1.1/24");
	dp_test_netlink_del_neigh("dp1T0", "1.1.1.2",
				  "aa:bb:cc:dd:1:a1");

} DP_END_TEST;

DP_START_TEST(ipv4_tcp_shadow, drop)
{
	struct dp_test_expected *test_exp = NULL;
	struct rte_mbuf *test_pak = NULL;

	struct dp_test_npf_rule_t rules[] = {
		{
			.rule     = "1",
			.pass     = BLOCK,
			.stateful = STATELESS,
			.npf      = "proto-final=6 src-port=41000"
						" rproc=markdscp(12)"},
		RULE_DEF_PASS,
		NULL_RULE };

	struct dp_test_npf_ruleset_t fw = {
		.rstype = "originate",
		.name   = "FW_TCP_ORIG",
		.enable = 1,
		.attach_point   = "dp1T0",
		.fwd    = FWD,
		.dir    = "out",
		.rules  = rules
	};
	dp_test_npf_fw_add(&fw, false);

	npf_orig_ipv4_tcp_shadow_setup(&test_exp, &test_pak);

	dp_test_exp_set_oif_name(test_exp, "dp1T0");
	dp_test_exp_set_fwd_status(test_exp, DP_TEST_FWD_DROPPED);

	/* Run the test.  kernel -> intf1 -> n1 */
	dp_test_send_slowpath_pkt(test_pak, test_exp);

	/* Verify firewall packet count */
	dp_test_npf_verify_rule_pkt_count(NULL, &fw, fw.rules[0].rule, 1);

	/* Cleanup */
	dp_test_npf_fw_del(&fw, false);

	dp_test_nl_del_ip_addr_and_connected("dp1T0", "1.1.1.1/24");
	dp_test_netlink_del_neigh("dp1T0", "1.1.1.2",
				  "aa:bb:cc:dd:1:a1");

} DP_END_TEST;

/*
 * Test creates ipv6 tcp packet and send it to shadow interface.
 * Originate firewall is configured in the interface to verify
 * dscp mark function and action drop.
 *
 *                                  |
 *                                  |
 *                                  v
 *                          +-----+ 2001::1/64
 *                          |     |
 *                          | uut |---------------host 2001::2
 *                          |     | dp1T0
 *                          +-----+ intf1
 *
 *              --> Forwards (on output)
 *              Source 2001::1 Destination 2001::2
 *
 */
DP_DECL_TEST_CASE(npf_orig, ipv6_tcp_shadow, NULL, NULL);

static void npf_orig_ipv6_tcp_shadow_setup(
		struct dp_test_expected **test_exp,
		struct rte_mbuf **test_pak)
{
	struct dp_test_pkt_desc_t *pkt;
	struct rte_mbuf *exp_pak;
	struct ip6_hdr *ip6;

	/* Setup interfaces and neighbors */
	dp_test_nl_add_ip_addr_and_connected("dp1T0", "2001::1/64");

	/*
	 * Simulate pkt from kernel to be tx on intf1
	 */
	struct dp_test_pkt_desc_t v4_pktA = {
		.text       = "Packet A, Local -> Neighbour 1",
		.len        = 20,
		.ether_type = RTE_ETHER_TYPE_IPV6,
		.l3_src     = "2001::1",
		.l2_src     = "0:0:0:0:0:0",
		.l3_dst     = "2001::2",
		.l2_dst     = "aa:bb:cc:dd:1:a1",
		.proto      = IPPROTO_TCP,
		.l4         = {
			.tcp = {
				.sport = 41000,
				.dport = 1000,
				.flags = 0
			}
		},
		.rx_intf    = "dp1T0",
		.tx_intf    = "dp1T0"
	};
	pkt = &v4_pktA;

	*test_pak = dp_test_from_spath_pkt_from_desc(pkt);

	*test_exp = dp_test_exp_create(*test_pak);
	exp_pak = dp_test_exp_get_pak_m(*test_exp, 0);
	ip6 = ip6hdr(exp_pak);
	dp_test_set_pak_ip6_field(ip6, DP_TEST_SET_TOS, IPTOS_DSCP_AF12);
}

DP_START_TEST(ipv6_tcp_shadow, dscp_remark)
{
	struct dp_test_expected *test_exp;
	struct rte_mbuf *test_pak;

	npf_orig_ipv6_tcp_shadow_setup(&test_exp, &test_pak);

	struct dp_test_npf_rule_t rules[] = {
		{
			.rule     = "1",
			.pass     = PASS,
			.stateful = STATELESS,
			.npf      = "proto-final=6 src-port=41000"
					" rproc=markdscp(12)"},
		RULE_DEF_BLOCK,
		NULL_RULE };

	struct dp_test_npf_ruleset_t fw = {
		.rstype = "originate",
		.name   = "FW_TCP_ORIG",
		.enable = 1,
		.attach_point   = "dp1T0",
		.fwd    = FWD,
		.dir    = "out",
		.rules  = rules
	};
	dp_test_npf_fw_add(&fw, false);

	dp_test_exp_set_oif_name(test_exp, "dp1T0");
	dp_test_exp_set_fwd_status(test_exp, DP_TEST_FWD_FORWARDED);

	/* Run the test.  kernel -> intf1 -> n1 */
	dp_test_send_slowpath_pkt(test_pak, test_exp);

	/* Verify firewall packet count */
	dp_test_npf_verify_rule_pkt_count(NULL, &fw, fw.rules[0].rule, 1);

	/* Cleanup */
	dp_test_npf_fw_del(&fw, false);

	dp_test_nl_del_ip_addr_and_connected("dp1T0", "2001::1/64");
} DP_END_TEST;

DP_START_TEST(ipv6_tcp_shadow, drop)
{
	struct dp_test_expected *test_exp;
	struct rte_mbuf *test_pak;

	npf_orig_ipv6_tcp_shadow_setup(&test_exp, &test_pak);

	struct dp_test_npf_rule_t rules[] = {
		{
			.rule     = "1",
			.pass     = BLOCK,
			.stateful = STATELESS,
			.npf      = "proto-final=6 src-port=41000"
						" rproc=markdscp(12)"},
		RULE_DEF_PASS,
		NULL_RULE };

	struct dp_test_npf_ruleset_t fw = {
		.rstype = "originate",
		.name   = "FW_TCP_ORIG",
		.enable = 1,
		.attach_point   = "dp1T0",
		.fwd    = FWD,
		.dir    = "out",
		.rules  = rules
	};
	dp_test_npf_fw_add(&fw, false);

	dp_test_exp_set_oif_name(test_exp, "dp1T0");
	dp_test_exp_set_fwd_status(test_exp, DP_TEST_FWD_DROPPED);

	/* Run the test.  kernel -> intf1 -> n1 */
	dp_test_send_slowpath_pkt(test_pak, test_exp);

	/* Verify firewall packet count */
	dp_test_npf_verify_rule_pkt_count(NULL, &fw, fw.rules[0].rule, 1);

	/* Cleanup */
	dp_test_npf_fw_del(&fw, false);

	dp_test_nl_del_ip_addr_and_connected("dp1T0", "2001::1/64");
} DP_END_TEST;

DP_DECL_TEST_CASE(npf_orig, ipv4_icmp_transit, NULL, NULL);

/*
 * Match on ICMP type and code
 * Test generate ICMP message upon packet too big with don't fragment
 * flag set
 *
 *                  2.2.2.2 +-----+ 1.1.1.1
 *                          |     |
 * host 2.2.2.1 ------------| uut |---------------host 1.1.1.2
 *                    dp3T3 |     | dp1T1 (mtu 1400)
 *                    intf1 +-----+ intf2
 *
 *
 *              --> Forwards (on output)
 *              Source 2.2.2.1 Destination 1.1.1.2 (length 1472, DSCP 0)
 *
 *                <-- Back ICMP
 *              Source 1.1.1.2 Destination 2.2.2.2
 */
static void npf_orig_ipv4_icmp_transit_setup(
		struct dp_test_expected **exp,
		struct rte_mbuf **test_pak)
{
	struct rte_mbuf *icmp_pak;
	const char *neigh3_mac_str = "aa:bb:cc:dd:ee:ff";
	const char *neigh1_mac_str = "bb:aa:cc:ee:dd:ff";
	struct iphdr *ip_inner;
	struct icmphdr *icph;
	struct iphdr *ip;
	int len = 1472;
	int icmplen;

	/* Set up the interface addresses */
	dp_test_nl_add_ip_addr_and_connected("dp1T1", "1.1.1.1/24");
	dp_test_nl_add_ip_addr_and_connected("dp3T3", "2.2.2.2/24");

	dp_test_netlink_set_interface_mtu("dp1T1", 1400);

	/* Add the nh arp we want the packet to follow */
	dp_test_netlink_add_neigh("dp3T3", "2.2.2.1", neigh3_mac_str);
	dp_test_netlink_add_neigh("dp1T1", "1.1.1.2", neigh1_mac_str);

	/* Create pak to match the route added above */
	*test_pak = dp_test_create_ipv4_pak("2.2.2.1", "1.1.1.2",
					   1, &len);
	ip = iphdr(*test_pak);
	dp_test_set_pak_ip_field(ip, DP_TEST_SET_DF, 1);

	(void)dp_test_pktmbuf_eth_init(*test_pak,
				       dp_test_intf_name2mac_str("dp3T3"),
				       neigh3_mac_str, RTE_ETHER_TYPE_IPV4);

	/*
	 * Expected packet
	 */
	/* Create expected icmp packet  */
	icmplen = sizeof(struct iphdr) + 576;
	icmp_pak = dp_test_create_icmp_ipv4_pak("2.2.2.2", "2.2.2.1",
						ICMP_DEST_UNREACH,
						ICMP_FRAG_NEEDED,
						DPT_ICMP_FRAG_DATA(1400),
						1, &icmplen,
						iphdr(*test_pak),
						&ip, &icph);

	(void)dp_test_pktmbuf_eth_init(icmp_pak,
				       neigh3_mac_str,
				       dp_test_intf_name2mac_str("dp3T3"),
				       RTE_ETHER_TYPE_IPV4);

	dp_test_set_pak_ip_field(ip, DP_TEST_SET_TOS,
				 IPTOS_PREC_INTERNETCONTROL);

	ip_inner = (struct iphdr *)(icph + 1);
	/*
	 * The TTL allowed to be changed from the original. From RFC
	 * 1812 s4.3.2.3:
	 *   The returned IP header (and user data) MUST be identical to
	 *   that which was received, except that the router is not
	 *   required to undo any modifications to the IP header that are
	 *   normally performed in forwarding that were performed before
	 *   the error was detected (e.g., decrementing the TTL, or
	 *   updating options)
	 */
	dp_test_set_pak_ip_field(ip_inner, DP_TEST_SET_TTL,
				 DP_TEST_PAK_DEFAULT_TTL - 1);

	ip = iphdr(icmp_pak);
	dp_test_set_pak_ip_field(ip, DP_TEST_SET_TOS, IPTOS_DSCP_AF12);

	*exp = dp_test_exp_create(icmp_pak);
	rte_pktmbuf_free(icmp_pak);
}

DP_START_TEST(ipv4_icmp_transit, packet_to_big_dscp_remark)
{
	struct dp_test_expected *exp;
	struct rte_mbuf *test_pak;

	npf_orig_ipv4_icmp_transit_setup(&exp, &test_pak);

	struct dp_test_npf_rule_t rules[] = {
		{
			.rule     = "1",
			.pass     = PASS,
			.stateful = STATELESS,
			.npf      = "proto-final=1 rproc=markdscp(12)"},
		RULE_DEF_BLOCK,
		NULL_RULE };

	struct dp_test_npf_ruleset_t fw = {
		.rstype = "originate",
		.name   = "FW_ICMPv4_ORIG",
		.enable = 1,
		.attach_point   = "dp3T3",
		.fwd    = FWD,
		.dir    = "out",
		.rules  = rules
	};
	dp_test_npf_fw_add(&fw, false);

	dp_test_exp_set_oif_name(exp, "dp3T3");

	/* now send test pak and check we get expected back */
	dp_test_pak_receive(test_pak, "dp3T3", exp);

	/* After test validations */
	dp_test_npf_verify_rule_pkt_count(NULL, &fw, fw.rules[0].rule, 1);

	/* Clean Up */
	dp_test_npf_fw_del(&fw, false);

	dp_test_netlink_del_neigh("dp3T3", "2.2.2.1", "aa:bb:cc:dd:ee:ff");
	dp_test_netlink_del_neigh("dp1T1", "1.1.1.2", "bb:aa:cc:ee:dd:ff");
	dp_test_nl_del_ip_addr_and_connected("dp1T1", "1.1.1.1/24");
	dp_test_nl_del_ip_addr_and_connected("dp3T3", "2.2.2.2/24");

	dp_test_netlink_set_interface_mtu("dp1T1", 1500);
} DP_END_TEST;

DP_START_TEST(ipv4_icmp_transit, drop)
{
	struct dp_test_expected *exp;
	struct rte_mbuf *test_pak;

	npf_orig_ipv4_icmp_transit_setup(&exp, &test_pak);

	struct dp_test_npf_rule_t rules[] = {
		{
			.rule     = "1",
			.pass     = BLOCK,
			.stateful = STATELESS,
			.npf      = "proto-final=1 rproc=markdscp(12)"},
		RULE_DEF_PASS,
		NULL_RULE };

	struct dp_test_npf_ruleset_t fw = {
		.rstype = "originate",
		.name   = "FW_ICMPv4_ORIG",
		.enable = 1,
		.attach_point   = "dp3T3",
		.fwd    = FWD,
		.dir    = "out",
		.rules  = rules
	};
	dp_test_npf_fw_add(&fw, false);

	dp_test_exp_set_oif_name(exp, "dp3T3");
	dp_test_exp_set_fwd_status(exp, DP_TEST_FWD_DROPPED);

	/* Run test */
	dp_test_pak_receive(test_pak, "dp3T3", exp);

	/* After test validations */
	dp_test_npf_verify_rule_pkt_count(NULL, &fw, fw.rules[0].rule, 1);

	/* Clean Up */
	dp_test_npf_fw_del(&fw, false);

	dp_test_netlink_del_neigh("dp3T3", "2.2.2.1", "aa:bb:cc:dd:ee:ff");
	dp_test_netlink_del_neigh("dp1T1", "1.1.1.2", "bb:aa:cc:ee:dd:ff");
	dp_test_nl_del_ip_addr_and_connected("dp1T1", "1.1.1.1/24");
	dp_test_nl_del_ip_addr_and_connected("dp3T3", "2.2.2.2/24");

	dp_test_netlink_set_interface_mtu("dp1T1", 1500);
} DP_END_TEST;
