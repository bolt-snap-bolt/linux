// SPDX-License-Identifier: GPL-2.0
/*
 * MaxLinear / Lantiq / Intel GSW switch driver for external MDIO-managed parts.
 * Currently only supports the GSW120 & GSW125.
 * 
 * See lantiq_gswip_core.c for additional information.
 *
 * Copyright (C) 2022 Reliable Controls Corporation,
 * 			Harley Sims <hsims@reliablecontrols.com>
 */

#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_mdio.h>

#include "lantiq_gsw.h"

#define RUN_MDIO_COMM_TESTS (1)

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

static inline void gsw_mdio_write_actual(struct mdio_device *mdio, u32 reg,
						u32 val)
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
	 * registers (0-30) are used to access internal addresses of
	 * (TBAR + 0-30)
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
				(val & cleared) == 0, sleep_us, timeout_us, \
				false, mdio, (reg_addr - tbar));
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

#if RUN_MDIO_COMM_TESTS
static bool gsw_mdio_comm_tests(struct gswip_priv *priv)
{
	struct mdio_device *mdio;
	void *reg_addr;
	u32 i, val, tbar, expected_tbar;

	mdio = ((struct gsw_mdio *)dev_get_drvdata(priv->dev))->mdio_dev;

	// basic TBAR r/w validation
	gsw_mdio_write_tbar(mdio, 0xABC);
	if (0xABC != gsw_mdio_read_tbar(mdio)) {
		printk("!RCC: TBAR r/w failed");
		return false;
	}

	// check TBAR only writes when necessary
	for (i = 0; i < 0xFFFF; i++) // 
	{
		tbar = gsw_mdio_check_write_tbar(mdio, i);
		expected_tbar = TARGET_BASE_ADDRESS_REG * (i / TARGET_BASE_ADDRESS_REG);
		if (tbar != expected_tbar) {
			printk("!RCC: TBAR sweep up failed: i:%d, tbar:%d, expected:%d", \
				i, tbar, expected_tbar);
			return false;
		}
	}

	for (i = 0xFFFF; i > 0; i--)
	{
		tbar = gsw_mdio_check_write_tbar(mdio, i);
		expected_tbar = TARGET_BASE_ADDRESS_REG * (i / TARGET_BASE_ADDRESS_REG);
		if (tbar != (TARGET_BASE_ADDRESS_REG * \
				(i / TARGET_BASE_ADDRESS_REG))) {
			printk("!RCC: TBAR sweep down failed: i:%d, tbar:%d, expected:%d", \
				i, tbar, expected_tbar);
			return false;
		}
	}

	// read some actual registers
	// these 4 sequential registers should all have reset values of 0x0000
	reg_addr = (void*)0xF380; // GPIO_OUT, GPIO_IN, GPIO_DIR, GPIO_ALTSEL0
	for (i = 0; i < 4; i++)
	{
		val = gsw_mdio_read(priv, reg_addr);
		if (val) {
			printk("!RCC: read failure: read %d from 0x%x", \
				val, (u32)reg_addr);
			return false;
		}
		reg_addr++;
	}
	// these 3 sequential registers should all have reset values of 0x7FFF
	reg_addr = (void*)0xF395; // GPIO2_OD, GPIO2_PUDSEL, GPIO_PUDEN
	for (i = 0; i < 3; i++)
	{
		val = gsw_mdio_read(priv, reg_addr);
		if (val != 0x7FFF) {
			printk("!RCC: read failure: read %d from 0x%x", \
				val, (u32)reg_addr);
			return false;
		}
		reg_addr++;
	}

	// do the same read tests using the poll timeout function
	// these 4 sequential registers should all have reset values of 0x0000
	reg_addr = (void*)0xF380; // GPIO_OUT, GPIO_IN, GPIO_DIR, GPIO_ALTSEL0
	for (i = 0; i < 4; i++)
	{
		// use same arguments as core driver
		val = gsw_mdio_read_timeout(priv, reg_addr, false, 20, 50000);
		if (val) {
			printk("!RCC: read_timeout failure: read %d from 0x%x", \
				val, (u32)reg_addr);
			return false;
		}
		reg_addr++;
	}
	// these 3 sequential registers should all have reset values of 0x7FFF
	reg_addr = (void*)0xF395; // GPIO2_OD, GPIO2_PUDSEL, GPIO_PUDEN
	for (i = 0; i < 3; i++)
	{
		// use same arguments as core driver
		val = gsw_mdio_read_timeout(priv, reg_addr, false, 20, 50000);
		if (val != 0x7FFF) {
			printk("!RCC: read_timeout failure: read %d from 0x%x", \
				val, (u32)reg_addr);
			return false;
		}
		reg_addr++;
	}

	// write a register
	reg_addr = (void*)0xF396; // GPIO2_PUDSEL
	for (i = 0; i < 0x7FFF; i++) // top bit is reserved
	{
		gsw_mdio_write(priv, reg_addr, i);
		val = gsw_mdio_read(priv, reg_addr);
		if (i != val) {
			printk("!RCC: write failure: read:%d, expected:%d", \
				val, i);
			return false;
		}
		gsw_mdio_write(priv, reg_addr, 0); //write zero each time to clear
	}

	/* TODO WARP-5828:
	 * - Verify reads/writes targeting PHYs on the GSW internal MDIO bus work as expected
	 * - Sub-in register defines for hard-coded addresses above
	 */

	return true;
}
#endif

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

#if RUN_MDIO_COMM_TESTS
	if (gsw_mdio_comm_tests(&mdio_data->common))
		printk("!RCC: GSW comm test PASS");
	else
		printk("!RCC: GSW comm test FAILURE");
#endif

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
MODULE_DESCRIPTION("MaxLinear GSW MDIO driver");
MODULE_LICENSE("GPL v2");
