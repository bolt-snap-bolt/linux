// SPDX-License-Identifier: GPL-2.0
/*
 * MaxLinear / Lantiq / Intel GSW switch driver for external MDIO-managed parts.
 * Currently only supports the GSW120 & GSW125.
 * 
 * See lantiq_gswip_core.c for additional information.
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
	/* TODO WARP-5829: write this function */
	return EIO;
}

static void gswip_switch_w(struct gswip_priv *priv, u32 val, u32 offset)
{
	/* TODO WARP-5829: write this function */
}

static u32 gswip_switch_r_timeout(struct gswip_priv *priv, u32 offset,
				  u32 cleared)
{
	/* TODO WARP-5829: write this function */
	return EIO;
}

static u32 gswip_slave_mdio_r(struct gswip_priv *priv, u32 offset)
{
	/* TODO WARP-5829: write this function */
	return EIO;
}

static void gswip_slave_mdio_w(struct gswip_priv *priv, u32 val, u32 offset)
{
	/* TODO WARP-5829: write this function */
}

static u32 gswip_mii_r(struct gswip_priv *priv, u32 offset)
{
	/* TODO WARP-5829: write this function */
	return EIO;
}

static void gswip_mii_w(struct gswip_priv *priv, u32 val, u32 offset)
{
	/* TODO WARP-5829: write this function */
}

/*-------------------------------------------------------------------------*/

static int gswip_mdio_probe(struct mdio_device *pmdiodev)
{
	/* TODO WARP-5829: write this function */
	dev_err(dev, "GSW120 SUPPORT NOT YET IMPLEMENTED\n");
	return EFAULT;
}

static int gsw_mdio_remove(struct mdio_device *pmdiodev)
{
	/* TODO WARP-5829: verify nothing else needs to be done in this function */
	return gsw_core_remove(mdiodev_get_drvdata(pmdiodev));
}

/*-------------------------------------------------------------------------*/

static const struct gsw_hw_info gsw_120 = {
	/* TODO WARP-5828: Determine what these values should be */
	.max_ports = 4,
	.cpu_port = 1,
	/* TODO WARP-5826: will also need .ops for newer kernel versions,
	 * where parts have had their DSA operations broken out.
	 */
};

static const struct of_device_id gsw_mdio_of_match[] = {
	{ .compatible = "maxlinear,gsw120", .data = &gsw_120 },
	{ .compatible = "maxlinear,gsw125", .data = &gsw_120 },
	{},
};

MODULE_DEVICE_TABLE(of, gsw_mdio_of_match);

static struct mdio_driver gsw_mdio_driver = {
	.probe  = gsw_mdio_probe,
	.remove = gsw_mdio_remove,
	.mdiodrv.driver = {
		.name = "gsw_mdio",
		.of_match_table = of_match_ptr(gsw_mdio_of_match),
	},
};

mdio_module_driver(gsw_mdio_driver);

MODULE_AUTHOR("Harley Sims <hsims@reliablecontrols.com>");
MODULE_DESCRIPTION("MaxLinear / Lantiq / Intel GSW MDIO driver");
MODULE_LICENSE("GPL v2");
