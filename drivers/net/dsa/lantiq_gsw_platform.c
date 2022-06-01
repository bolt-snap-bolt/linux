// SPDX-License-Identifier: GPL-2.0
/*
 * Lantiq / Intel GSWIP switch driver for VRX200 SoCs
 *
 * Copyright (C) 2010 Lantiq Deutschland
 * Copyright (C) 2012 John Crispin <john@phrozen.org>
 * Copyright (C) 2017 - 2019 Hauke Mehrtens <hauke@hauke-m.de>
 * Copyright (C) 2022 Reliable Controls Corporation,
 * 			Harley Sims <hsims@reliablecontrols.com>
 */

#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>

#include "lantiq_gsw.h"

struct gsw_platform {
	struct platform_device *platform_dev;
	struct gswip_priv common;
};

static u32 gsw_platform_read(struct gswip_priv *priv, void *addr)
{
	(void*)priv;
	return __raw_readl(addr);
}

static u32 gsw_platform_read_timeout(struct gswip_priv *priv, void *addr, 
				u32 cleared, u32 sleep_us, u32 timeout_us)
{
	u32 val;
	
	(void*)priv;
	return readx_poll_timeout(__raw_readl, addr, val,
				(val & cleared) == 0, sleep_us, timeout_us);
}

static void gsw_platform_write(struct gswip_priv *priv, void *addr, u32 val)
{
	(void*)priv;
	__raw_writel(val, addr);
}

static const struct gsw_ops gsw_platform_ops = {
	.read = gsw_platform_read,
	.read_timeout = gsw_platform_read_timeout,
	.write = gsw_platform_write,
};

/*-------------------------------------------------------------------------*/

static int gsw_platform_probe(struct platform_device *pdev)
{
	struct device *dev = &(pdev->dev);
	struct gsw_platform *platform_data;
	int err;

	platform_data = devm_kzalloc(dev, sizeof(*platform_data), GFP_KERNEL);
	if (!platform_data)
		return -ENOMEM;

	platform_data->common.gswip = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(platform_data->common.gswip))
		return PTR_ERR(platform_data->common.gswip);

	platform_data->common.mdio = devm_platform_ioremap_resource(pdev, 1);
	if (IS_ERR(platform_data->common.mdio))
		return PTR_ERR(platform_data->common.mdio);

	platform_data->common.mii = devm_platform_ioremap_resource(pdev, 2);
	if (IS_ERR(platform_data->common.mii))
		return PTR_ERR(platform_data->common.mii);

	err = gsw_core_probe(&platform_data->common, dev);
	if (err)
		return err;

	platform_set_drvdata(pdev, platform_data);

	return 0;
}

static int gsw_platform_remove(struct platform_device *pdev)
{
	struct gsw_platform *platform_data = platform_get_drvdata(pdev);

	return gsw_core_remove(&platform_data->common);
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
