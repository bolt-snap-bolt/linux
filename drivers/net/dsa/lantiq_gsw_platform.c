// SPDX-License-Identifier: GPL-2.0
/*
 * Lantiq / Intel GSWIP switch driver for VRX200 SoCs
 *
 * Copyright (C) 2010 Lantiq Deutschland
 * Copyright (C) 2012 John Crispin <john@phrozen.org>
 * Copyright (C) 2017 - 2019 Hauke Mehrtens <hauke@hauke-m.de>
 * Copyright (C) 2022 Reliable Controls Corporation, Harley Sims <hsims@reliablecontrols.com>
 */

/* TODO WARP-5829: determine how many of these includes I can delete */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/etherdevice.h>
#include <linux/firmware.h>
#include <linux/if_bridge.h>
#include <linux/if_vlan.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/phylink.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <net/dsa.h>
#include <dt-bindings/mips/lantiq_rcu_gphy.h>

#include "lantiq_pce.h"

static u32 gswip_switch_r(struct gswip_priv *priv, u32 offset)
{
	return __raw_readl(priv->gswip + (offset * 4));
}

static void gswip_switch_w(struct gswip_priv *priv, u32 val, u32 offset)
{
	__raw_writel(val, priv->gswip + (offset * 4));
}

static u32 gswip_switch_r_timeout(struct gswip_priv *priv, u32 offset,
				  u32 cleared)
{
	u32 val;

	return readx_poll_timeout(__raw_readl, priv->gswip + (offset * 4), val,
				  (val & cleared) == 0, 20, 50000);
}

static u32 gswip_slave_mdio_r(struct gswip_priv *priv, u32 offset)
{
	return __raw_readl(priv->mdio + (offset * 4));
}

static void gswip_slave_mdio_w(struct gswip_priv *priv, u32 val, u32 offset)
{
	__raw_writel(val, priv->mdio + (offset * 4));
}

static u32 gswip_mii_r(struct gswip_priv *priv, u32 offset)
{
	return __raw_readl(priv->mii + (offset * 4));
}

static void gswip_mii_w(struct gswip_priv *priv, u32 val, u32 offset)
{
	__raw_writel(val, priv->mii + (offset * 4));
}

/*-------------------------------------------------------------------------*/

static int gsw_platform_probe(struct platform_device *pdev)
{
	/* TODO WARP-5829:
	 * - move platform-specific parts of gsw_core_probe() out to here
	 */
	return gsw_core_probe(pdev);
}

static int gsw_platform_remove(struct platform_device *pdev)
{
	return gsw_core_remove(platform_get_drvdata(pdev));
}

/*-------------------------------------------------------------------------*/

static const struct gsw_hw_info gswip_xrx200 = {
	.max_ports = 7,
	.cpu_port = 6,
};

static const struct of_device_id gsw_platform_of_match[] = {
	{ .compatible = "lantiq,xrx200-gswip", .data = &gswip_xrx200 },
	{},
};
MODULE_DEVICE_TABLE(of, gsw_platform_of_match);

static struct platform_driver gsw_platform_driver = {
	.probe = gsw_platform_probe,
	.remove = gsw_platform_remove,
	.driver = {
		.name = "gsw_platform",
		.of_match_table = gsw_platform_of_match,
	},
};

module_platform_driver(gsw_platform_driver);

MODULE_AUTHOR("Hauke Mehrtens <hauke@hauke-m.de>");
MODULE_DESCRIPTION("Lantiq / Intel GSWIP driver");
MODULE_LICENSE("GPL v2");
