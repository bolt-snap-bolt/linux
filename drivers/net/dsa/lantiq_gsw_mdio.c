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
 * Copyright (C) 2022 Reliable Controls Corporation,
 * 						Harley Sims <hsims@reliablecontrols.com>
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

#include "lantiq_gsw.h"
#include "lantiq_pce.h"

#define NUM_ACCESSIBLE_REGS (30)
#define TARGET_BASE_ADDRESS_REG (31)

struct gsw_mdio {
	struct mdio_device *mdio_dev;
	struct gswip_priv common;
};

static inline u32 gsw_mdio_read_actual(struct mdio_device *mdio, u32 reg)
{
	return mdio->bus->read(mdio->bus, mdio->addr, reg);
}

static inline void gsw_mdio_write_actual(struct mdio_device *mdio, u32 reg, u32 val)
{
	mdio->bus->write(mdio->bus, mdio->addr, reg, val);
}

static inline u32 gsw_mdio_read_tbar(struct mdio_device *mdio)
{
	return gsw_mdio_read_actual(mdio, TARGET_BASE_ADDRESS_REG);
}

static inline void gsw_mdio_write_tbar(struct mdio_device *mdio, u32 reg_addr)
{
	gsw_mdio_write_actual(mdio, TARGET_BASE_ADDRESS_REG, reg_addr);
}

static u32 gsw_mdio_check_write_tbar(struct mdio_device *mdio, u32 reg_addr)
{
	u32 tbar = gsw_mdio_read_tbar(mdio);

	/* MDIO slave interface uses an indirect addressing scheme that allows
	 * access to NUM_ACCESSIBLE_REGS registers at a time. The Target Base
	 * Address Register (TBAR) is used to set a base offset, then MDIO
	 * registers (0-30) are used to access internal addresses of (TBAR + 0-30)
	 */
	if ((reg_addr < tbar) || (reg_addr > (tbar + NUM_ACCESSIBLE_REGS))) {
			gsw_mdio_write_tbar(mdio, reg_addr);
			tbar = reg_addr;
	}

	return tbar;
}

static u32 gsw_mdio_read(struct gswip_priv *priv, void *addr)
{
	struct mdio_device *mdio;
	u32 reg_addr, tbar, val;

	mdio = ((struct gsw_mdio *)dev_get_drvdata(priv->dev))->mdio_dev;
	reg_addr = (u32)addr;

	mutex_lock(&mdio->bus->mdio_lock);
	tbar = gsw_mdio_check_write_tbar(mdio, reg_addr);
	val = gsw_mdio_read_actual(mdio, (reg_addr - tbar));
	mutex_unlock(&mdio->bus->mdio_lock);

	return val;
}

static u32 gsw_mdio_read_timeout(struct gswip_priv *priv, void *addr,
								u32 cleared, u32 sleep_us, u32 timeout_us)
{
	struct mdio_device *mdio;
	u32 retval, reg_addr, tbar, val;

	mdio = ((struct gsw_mdio *)dev_get_drvdata(priv->dev))->mdio_dev;
	reg_addr = (u32)addr;

	mutex_lock(&mdio->bus->mdio_lock);
	tbar = gsw_mdio_check_write_tbar(mdio, reg_addr);
	retval = read_poll_timeout(gsw_mdio_read_actual, val, \
				(val & cleared) == 0, sleep_us, timeout_us, false, \
				mdio, (reg_addr - tbar));
	mutex_unlock(&mdio->bus->mdio_lock);

	return retval;
}

static void gsw_mdio_write(struct gswip_priv *priv, void *addr, u32 val)
{
	struct mdio_device *mdio;
	u32 reg_addr, tbar;

	mdio = ((struct gsw_mdio *)dev_get_drvdata(priv->dev))->mdio_dev;
	reg_addr = (u32)addr;

	mutex_lock(&mdio->bus->mdio_lock);
	tbar = gsw_mdio_check_write_tbar(mdio, reg_addr);
	gsw_mdio_write_actual(mdio, (reg_addr - tbar), val);
	mutex_unlock(&mdio->bus->mdio_lock);
}

static const struct gsw_ops gsw_mdio_ops = {
	.read = gsw_mdio_read,
	.read_timeout = gsw_mdio_read_timeout,
	.write = gsw_mdio_write,
};

/*-------------------------------------------------------------------------*/

static int gsw_mdio_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &(mdiodev->dev);
	struct gsw_mdio *mdio_data;
	int err;

	mdio_data = devm_kzalloc(dev, sizeof(*mdio_data), GFP_KERNEL);
	if (!mdio_data)
		return -ENOMEM;

	err = gsw_core_probe(&mdio_data->common, dev);
	if (err)
		return err;

	dev_set_drvdata(dev, mdio_data);

	return 0;
}

static void gsw_mdio_remove(struct mdio_device *pmdiodev)
{
	struct gsw_mdio *mdio_data = mdiodev_get_drvdata(pmdiodev);
	gsw_core_remove(&mdio_data->common);
}

/*-------------------------------------------------------------------------*/

/* MaxLinear GSW120 & GSW125 */
static const struct gsw_hw_info gsw_120 = {
	/* TODO WARP-5828: Determine what these values should be */
	.max_ports = 4,
	.cpu_port = 1,
};

static const struct of_device_id gsw_mdio_of_match[] = {
	{ .compatible = "maxlinear,gsw12x", .data = &gsw_120 },
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
