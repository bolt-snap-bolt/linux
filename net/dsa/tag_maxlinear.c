// SPDX-License-Identifier: GPL-2.0
/*
 * MaxLinear special tag support
 *
 * Copyright (C) 2022 Reliable Controls Corporation,
 * 			Harley Sims <hsims@reliablecontrols.com>
 */

#include <linux/bitops.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <net/dsa.h>

#include "dsa_priv.h"

/* 
 * Default Ethertype of special tag data is 0x88C3.
 * Value is configurable via switch registers.
 */
#define ML_SPECIAL_TAG_ETHERTYPE_B1	(0x88)
#define ML_SPECIAL_TAG_ETHERTYPE_B2	(0xC3)

/* special tag in TX direction, i.e. ingress special tag */
#define MAXLINEAR_TX_HEADER_LEN		(8)
/* Byte 0 & Byte 1: special Ethertype (ML_SPECIAL_TAG_ETHERTYPE) */
/* Following 6 bytes are the actual tag content */
/* Byte 2 */
#define ML_TX_PORT_MAP_EN		BIT(7)
#define ML_TX_TRAFF_CLASS_EN		BIT(6)
#define ML_TX_TIME_STAMP_EN		BIT(5)
#define ML_TX_FORCE_NO_LRN		BIT(4)
#define ML_TX_TRAFF_CLASS_SHIFT		0
#define ML_TX_TRAFF_CLASS_MASK		GENMASK(3, 0)
/* Byte 3 - Egress port bitmap */
/* assigned to via BIT(port), where port is zero-indexed */
#define ML_TX_EGRESS_PORT_MAP_SHIFT	0
#define ML_TX_EGRESS_PORT_MAP_MASK	GENMASK(7, 0)
/* Byte 4 - all reserved */
/* Byte 5 */
#define ML_TX_INT_EN			BIT(4)
#define ML_TX_SRC_PORT_SHIFT		0
#define ML_TX_SRC_PORT_MASK		GENMASK(3, 0)
/* Byte 6 & Byte 7 - all reserved */

/* special tag in RX direction, i.e. egress special tag */
#define MAXLINEAR_RX_HEADER_LEN		(8)
/* Byte 0 & Byte 1: special Ethertype (ML_SPECIAL_TAG_ETHERTYPE) */
/* Following 6 bytes are the actual tag content */
/* Byte 2 */
#define ML_RX_TRAFF_CLASS_SHIFT		4
#define ML_RX_TRAFF_CLASS_MASK		GENMASK(7, 4)
#define ML_RX_INGRESS_PORT_NUM_SHIFT	0
#define ML_RX_INGRESS_PORT_NUM_MASK	GENMASK(3, 0)
/* Byte 3 */
#define ML_RX_PPPOE_PKT			BIT(7)
#define ML_RX_IPV4_PKT			BIT(6)
#define ML_RX_IP_OFFSET_SHIFT		0
#define ML_RX_IP_OFFSET_MASK		GENMASK(5, 0)
/* Byte 4 */
#define ML_RX_DEST_PORT_MAP_SHIFT	0
#define ML_RX_DEST_PORT_MAP_MASK	GENMASK(7, 0)
/* Byte 5 - all reserved */
/* Byte 6 */
#define ML_RX_MIRRORED			BIT(7)
#define ML_RX_KNOWN_L2_ENTRY		BIT(6)
#define ML_RX_PKT_LEN_HIGH_SHIFT	0
#define ML_RX_PKT_LEN_HIGH_MASK		GENMASK(5, 0)
/* Byte 7 */
#define ML_RX_PKT_LEN_LOW_SHIFT		0
#define ML_RX_PKT_LEN_LOW_MASK		GENMASK(7, 0)

static struct sk_buff *ML_tag_xmit(struct sk_buff *skb,
				      struct net_device *dev)
{
	struct dsa_port *dp = dsa_slave_to_port(dev);
	u8 *ml_ingress_tag;
	
	skb_push(skb, MAXLINEAR_TX_HEADER_LEN);
	dsa_alloc_etype_header(skb, MAXLINEAR_TX_HEADER_LEN);

	/* fill in tag data in extra space we've created */
	ml_ingress_tag = dsa_etype_header_pos_tx(skb);
	ml_ingress_tag[0] = ML_SPECIAL_TAG_ETHERTYPE_B1;
	ml_ingress_tag[1] = ML_SPECIAL_TAG_ETHERTYPE_B2;
	ml_ingress_tag[2] = ML_TX_PORT_MAP_EN | ML_TX_FORCE_NO_LRN;
	ml_ingress_tag[3] = BIT(dp->index) & ML_TX_EGRESS_PORT_MAP_MASK;
	ml_ingress_tag[4] = 0;
	ml_ingress_tag[5] = 0;
	ml_ingress_tag[6] = 0;
	ml_ingress_tag[7] = 0;

	return skb;
}

static struct sk_buff *ML_tag_rcv(struct sk_buff *skb,
				     struct net_device *dev)
{
	int port;
	u8 *ml_egress_tag;

	if (unlikely(!pskb_may_pull(skb, MAXLINEAR_RX_HEADER_LEN)))
		return NULL;

	ml_egress_tag = dsa_etype_header_pos_rx(skb);

	if ((ml_egress_tag[0] != ML_SPECIAL_TAG_ETHERTYPE_B1)
		|| (ml_egress_tag[1] != ML_SPECIAL_TAG_ETHERTYPE_B2))
		return NULL;
	
	/* Get source port information */
	port = (ml_egress_tag[2] & ML_RX_INGRESS_PORT_NUM_MASK) \
						>> ML_RX_INGRESS_PORT_NUM_SHIFT;
	skb->dev = dsa_master_find_slave(dev, 0, port);
	if (!skb->dev)
		return NULL;

	/* mark the packet as having been forwarded in HW or not */
	dsa_default_offload_fwd_mark(skb);

	/* remove special tag and move MAC addresses to right spot */
	skb_pull_rcsum(skb, MAXLINEAR_RX_HEADER_LEN);
	dsa_strip_etype_header(skb, MAXLINEAR_RX_HEADER_LEN);

	return skb;
}

static const struct dsa_device_ops maxlinear_netdev_ops = {
	.name = "maxlinear-gsw",
	.proto	= DSA_TAG_PROTO_MAXLINEAR,
	.xmit = ML_tag_xmit,
	.rcv = ML_tag_rcv,
	.needed_headroom = MAXLINEAR_RX_HEADER_LEN,
};

MODULE_LICENSE("GPL");
MODULE_ALIAS_DSA_TAG_DRIVER(DSA_TAG_PROTO_MAXLINEAR);

module_dsa_tag_driver(maxlinear_netdev_ops);
