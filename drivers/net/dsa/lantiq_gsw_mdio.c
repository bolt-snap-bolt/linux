// SPDX-License-Identifier: GPL-2.0
/*
 * MaxLinear / Intel GSW switch driver for external MDIO-managed parts.
 * Currently supports the GSW120, GSW125, and GSW140.
 * 
 * See lantiq_gsw_core.c for additional information.
 *
 * Copyright (C) 2022 Reliable Controls Corporation,
 * 			Harley Sims <hsims@reliablecontrols.com>
 */

#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of_mdio.h>

#include "lantiq_gsw.h"

#if RCC_GSW_RUN_MDIO_COMM_TESTS
#include <net/dsa.h>
#endif

#define NUM_ACCESSIBLE_REGS 		(30)
#define TARGET_BASE_ADDRESS_REG 	(31)
#define GSW_REG_BASE_OFFSET_SWITCH 	(0xE000)
#define GSW_REG_BASE_OFFSET_MDIO 	(0xF400)
#define GSW_REG_BASE_OFFSET_UNUSED 	(0x0000)
#define GSW_REG_MII_CFG5		(0xF100)
#define GSW_REG_MII_PCDU5		(0xF101)

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

static u32 gsw_mdio_calculate_reg_addr(struct gswip_priv *priv, \
					void *base, u32 offset)
{
	if (base == priv->gswip) {
		if (offset == GSWIP_SWRES)
			/* SWRES is at mdio base on MaxLinear parts */
			return (u32)priv->mdio + offset;
		else
			return (u32)base + offset;
	} else if (base == priv->mdio) {
		return (u32)base + offset;
	} else {
		/* covers base = priv->mii, equivalent to base = 0x00 */
		switch (offset) {
		case GSWIP_MII_CFGp(5):
			return GSW_REG_MII_CFG5;
		case GSWIP_MII_PCDU5:
			return GSW_REG_MII_PCDU5;
		case GSWIP_MII_PCDU0:
		case GSWIP_MII_PCDU1:
			/* gsw_mdio_check_interface_support() prevents
			 * ports other than 5 from being configured as RGMII,
			 * which in turn should prevents the core driver logic
			 * from ever attempting to set these PCDU0/1 registers,
			 * as they are RGMII-specific.
			 */
		default:
			/* None of the other MII base registers referred to by
			* the core driver logic exist on MaxLinear parts.
			* Return 0 and do not perform any R/W operation.
			*/
			return 0;
		}
	} 
}

static u32 gsw_mdio_read(struct gswip_priv *priv, void *base, u32 offset)
{
	struct mdio_device *mdio;
	u32 reg_addr, tbar;
	u32 val = 0;

	mdio = ((struct gsw_mdio *)dev_get_drvdata(priv->dev))->mdio_dev;
	reg_addr = gsw_mdio_calculate_reg_addr(priv, base, offset);

	if (reg_addr != 0) {
		mutex_lock(&mdio->bus->mdio_lock);
		tbar = gsw_mdio_check_write_tbar(mdio, reg_addr);
		val = gsw_mdio_read_actual(mdio, (reg_addr - tbar));
		mutex_unlock(&mdio->bus->mdio_lock);
	}

	return val;
}

static int gsw_mdio_poll_timeout(struct gswip_priv *priv, void *base, \
			u32 offset, u32 cleared, u32 sleep_us, u32 timeout_us)
{
	struct mdio_device *mdio;
	u32 reg_addr, tbar, val;
	int retval = -ENXIO;

	mdio = ((struct gsw_mdio *)dev_get_drvdata(priv->dev))->mdio_dev;
	reg_addr = gsw_mdio_calculate_reg_addr(priv, base, offset);

	if (reg_addr != 0) {
		mutex_lock(&mdio->bus->mdio_lock);
		tbar = gsw_mdio_check_write_tbar(mdio, reg_addr);
		retval = read_poll_timeout(gsw_mdio_read_actual, val, \
					(val & cleared) == 0, sleep_us, timeout_us, \
					false, mdio, (reg_addr - tbar));
		mutex_unlock(&mdio->bus->mdio_lock);
	}

	return retval;
}

static void gsw_mdio_write(struct gswip_priv *priv, void *base, \
				u32 offset, u32 val)
{
	struct mdio_device *mdio;
	u32 reg_addr, tbar;

	mdio = ((struct gsw_mdio *)dev_get_drvdata(priv->dev))->mdio_dev;
	reg_addr = gsw_mdio_calculate_reg_addr(priv, base, offset);

	if (reg_addr != 0) {
		mutex_lock(&mdio->bus->mdio_lock);
		tbar = gsw_mdio_check_write_tbar(mdio, reg_addr);
		gsw_mdio_write_actual(mdio, (reg_addr - tbar), val);
		mutex_unlock(&mdio->bus->mdio_lock);
	}
}

static bool gsw_mdio_check_interface_support(int port, phy_interface_t interface)
{
	switch (port) {
	case 0:
	case 1:
	case 2:
	case 3:
		if (interface != PHY_INTERFACE_MODE_INTERNAL)
			return false;
		break;
	case 4:
		if (interface != PHY_INTERFACE_MODE_SGMII)
			return false;
		break;
	case 5:
		if (!phy_interface_mode_is_rgmii(interface))
			return false;
		break;
	default:
		return false;
	}

	return true;
}

/* RCC Edit:
 * Add sysfs interface to plumb output of a register read that indicates
 * whether part has been successfully configured by the driver or not,
 * for analysis by the WARP application.
 */
static ssize_t show_rcc_phy_esd(struct device *dev, \
				struct device_attribute *attr, char *buf)
{
	struct gswip_priv *priv = &(((struct gsw_mdio *)dev_get_drvdata(dev))->common);
	const char* msg;
	u32 data;

	data = gsw_mdio_read(priv, priv->gswip, GSWIP_PCE_GCTRL_1);
	switch (data) {
		/* Configured condition:
		 * GSWIP_PCE_GCTRL_1_VLANMD, set by gswip_setup()
		 * GSWIP_PCE_GCTRL_1_MAC_LRN_MOD, set on reset
		 * other bits 0
		 */
		case (GSWIP_PCE_GCTRL_1_VLANMD | GSWIP_PCE_GCTRL_1_MAC_LRN_MOD):
			msg = "ok";
			break;
		/* Reset condition:
		 * GSWIP_PCE_GCTRL_1_MAC_LRN_MOD, set on reset
		 * other bits 0
		 */
		case (GSWIP_PCE_GCTRL_1_MAC_LRN_MOD):
			msg = "reset";
			break;
		case 0xFFFFFFFF:
			msg = "lockup";
			break;
		default:
			msg = "unknown";
			break;
	}
	return sprintf(buf, "%s\n", msg);
}
static const DEVICE_ATTR(rcc_phy_esd, 0444, show_rcc_phy_esd, NULL);
/* END RCC EDIT */

/*-------------------------------------------------------------------------*/
#if RCC_GSW_ENABLE_BREAKPOINTS
// Defines required for breakpoint functionality
#define GSW_REG_OFFSET_GPIO2_IN		(0x1391) // 0xF391 = priv->gswip + 0x1391
#define GSW_REG_OFFSET_GPIO2_DIR	(0x1392) // 0xF392 = priv->gswip + 0x1392
#define GSW_REG_OFFSET_GPIO2_ALTSEL0	(0x1393) // 0xF393 = priv->gswip + 0x1393
#define GSW_REG_OFFSET_GPIO2_ALTSEL1	(0x1394) // 0xF394 = priv->gswip + 0x1394
#define GSWIP_GPIO2_b14_GPIO30_MASK	(0x4000)

static void gsw_mdio_init_breakpoints(struct gswip_priv *priv)
{	
	u16 reg_val;

	// Turn GPIO30 into an input pin to use it as a play button
	// Clear bit in both ALTSEL registers to configure for GPIO function
	reg_val = gsw_mdio_read(priv, priv->gswip, GSW_REG_OFFSET_GPIO2_ALTSEL0);
	reg_val &= ~(GSWIP_GPIO2_b14_GPIO30_MASK);
	gsw_mdio_write(priv, priv->gswip, GSW_REG_OFFSET_GPIO2_ALTSEL0, reg_val);
	reg_val = gsw_mdio_read(priv, priv->gswip, GSW_REG_OFFSET_GPIO2_ALTSEL1);
	reg_val &= ~(GSWIP_GPIO2_b14_GPIO30_MASK);
	gsw_mdio_write(priv, priv->gswip, GSW_REG_OFFSET_GPIO2_ALTSEL1, reg_val);

	// Clear bit in direction register to configure for input
	reg_val = gsw_mdio_read(priv, priv->gswip, GSW_REG_OFFSET_GPIO2_DIR);
	reg_val &= ~(GSWIP_GPIO2_b14_GPIO30_MASK);
	gsw_mdio_write(priv, priv->gswip, GSW_REG_OFFSET_GPIO2_DIR, reg_val);

	// Verify expected pull-up behaviour by reading a 1
	reg_val = gsw_mdio_read(priv, priv->gswip, GSW_REG_OFFSET_GPIO2_IN);
	if ((reg_val & GSWIP_GPIO2_b14_GPIO30_MASK) != 0)
		printk("!RCC: GSW breakpoint system ONLINE");
	else
		printk("!RCC: WARNING: Could not verify GSW breakpoint functionality.");
}

static void gsw_mdio_breakpoint(struct gswip_priv *priv, const char* func_name, int line)
{
	u16 reg_val;
	bool play = false;

	printk("!RCC: BKPT %s ln %d \n", func_name, line);

	while (!play)
	{
		usleep_range(10*1000, 100*1000);
		reg_val = gsw_mdio_read(priv, priv->gswip, GSW_REG_OFFSET_GPIO2_IN);
		play = ((reg_val & GSWIP_GPIO2_b14_GPIO30_MASK) == 0) ? true : false;
	}

	usleep_range(10*1000, 50*1000);
}
#endif
/*-------------------------------------------------------------------------*/

static const struct gsw_hw_ops gsw_mdio_ops = {
	.read 				= gsw_mdio_read,
	.write 				= gsw_mdio_write,
	.poll_timeout 			= gsw_mdio_poll_timeout,
	.check_interface_support 	= gsw_mdio_check_interface_support,
#if RCC_GSW_ENABLE_BREAKPOINTS
	.breakpoint			= gsw_mdio_breakpoint,
#endif
};

/*-------------------------------------------------------------------------*/

#if RCC_GSW_RUN_MDIO_COMM_TESTS
#define GSW_REG_OFFSET_GPIO_OUT		(0x1380) // 0xF380 = priv->gswip + 0x1380
#define GSW_REG_OFFSET_GPIO_PUDSEL	(0x1386) // 0xF386 = priv->gswip + 0x1386
#define GSW_REG_OFFSET_GPIO2_OD		(0x1395) // 0xF395 = priv->gswip + 0x1395
#define GSW_REG_OFFSET_GPIO2_PUDSEL	(0x1396) // 0xF396 = priv->gswip + 0x1396
#define GSW_REG_OFFSET_GPIO2_PUDEN	(0x1397) // 0xF397 = priv->gswip + 0x1397
#define GSW_REG_OFFSET_MSPI_DIN45	(0x151A) // 0xF51A = priv->gswip + 0x151A

#define MDIO_PHY_REG_FWV		(0x1E)
/* Expected PHY FW version determined experimentally (i.e. by reading) */
#define MDIO_PHY_EXPECTED_FWV		(0x8548)

#define MDIO_PHY_REG_LED_CTRL		(0x1B)
#define MDIO_PHY_LED_CTRL_RESET_VAL	(0x0F00)
/* disable normal LED functionality, manually switch LEDS on */
#define MDIO_PHY_LED_CTRL_MANUAL_ON	(0x000F)
static bool gsw_mdio_comm_tests(struct gswip_priv *priv)
{
	struct mdio_device *mdio;
	u32 reg_addr, reg_addr_2, reg_addr_3, \
		i, val, tbar, expected_tbar, \
		mask, err;

	mdio = ((struct gsw_mdio *)dev_get_drvdata(priv->dev))->mdio_dev;

	// basic TBAR r/w validation
	gsw_mdio_write_tbar(mdio, 0xABC);
	if (0xABC != gsw_mdio_read_tbar(mdio)) {
		printk("!RCC: TBAR r/w failed");
		return false;
	}

	// basic read validation (check some registers against reset values)
	reg_addr = GSW_REG_OFFSET_GPIO_OUT; // reset value of 0x0000
	val = gsw_mdio_read(priv, priv->gswip, reg_addr);
	if (val != 0) {
		printk("!RCC: read failure: read %d from 0x%x", \
			val, (u32)reg_addr);
		return false;
	}
	reg_addr = GSW_REG_OFFSET_GPIO2_OD; // reset value of 0x7FFF
	val = gsw_mdio_read(priv, priv->gswip, reg_addr);
	if (val != 0x7FFF) {
		printk("!RCC: read failure: read %d from 0x%x", \
			val, (u32)reg_addr);
		return false;
	}

	// basic validation of poll timeout function
	reg_addr = GSW_REG_OFFSET_GPIO_OUT; // reset value of 0x0000
	mask = 0xFFFF;
	// use same timing arguments as core driver
	val = gsw_mdio_poll_timeout(priv, priv->gswip, reg_addr, mask, 20, 50000);
	if (val != 0) { // expect success (val = 0)
		printk("!RCC: poll_timeout failure: retval:0x%x reading 0x%x w mask 0x%x", \
			val, (u32)reg_addr, mask);
		return false;
	}
	reg_addr = GSW_REG_OFFSET_GPIO2_OD; // reset value of 0x7FFF
	mask = 0x8000;
	// use same timing arguments as core driver
	val = gsw_mdio_poll_timeout(priv, priv->gswip, reg_addr, mask, 20, 50000);
	if (val != 0) { // expect success (val = 0)
		printk("!RCC: poll_timeout failure: retval:0x%x reading 0x%x w mask 0x%x", \
			val, (u32)reg_addr, mask);
		return false;
	}
	mask = 0x7FFF;
	val = gsw_mdio_poll_timeout(priv, priv->gswip, reg_addr, mask, 20, 50000);
	if (val != -ETIMEDOUT) { // expect timeout (val = -ETIMEDOUT)
		printk("!RCC: poll_timeout failure: retval:0x%x reading 0x%x w mask 0x%x", \
			val, (u32)reg_addr, mask);
		return false;
	}

	// check TBAR only writes when necessary
	for (i = 0; i < 0xFFFF; i++) // 
	{
		tbar = gsw_mdio_check_write_tbar(mdio, i);
		expected_tbar = TARGET_BASE_ADDRESS_REG * \
					(i / TARGET_BASE_ADDRESS_REG);
		if (tbar != expected_tbar) {
			printk("!RCC: TBAR sweep up failed: i:%d, tbar:%d, expected:%d", \
				i, tbar, expected_tbar);
			return false;
		}
	}
	gsw_mdio_write_tbar(mdio, 0);
	for (i = 0xFFFF; i > 0; i--)
	{
		tbar = gsw_mdio_check_write_tbar(mdio, i);
		// we are sweeping down, so tbar will change every time
		if (tbar != i) {
			printk("!RCC: TBAR sweep down failed: i:%d, tbar:%d, expected:%d", \
				i, tbar, expected_tbar);
			return false;
		}
	}

	// write validation: write all acceptable values to a register
	reg_addr = GSW_REG_OFFSET_GPIO2_PUDSEL;
	for (i = 0; i < 0x7FFF; i++) // top bit is reserved
	{
		gsw_mdio_write(priv, priv->gswip, reg_addr, i);
		val = gsw_mdio_read(priv, priv->gswip, reg_addr);
		if (i != val) {
			printk("!RCC: write failure: read:0x%x, expected:0x%x", \
				val, i);
			return false;
		}
		gsw_mdio_write(priv, priv->gswip, reg_addr, 0); //write zero to clear
	}

	// write validation: read & write at all NUM_ACCESSIBLE_REGS places
	reg_addr = GSW_REG_OFFSET_GPIO2_PUDEN;
	tbar = (u32)priv->gswip + reg_addr;
	for (i = 0; i <= NUM_ACCESSIBLE_REGS; i++)
	{
		gsw_mdio_write_tbar(mdio, tbar);
		gsw_mdio_write(priv, priv->gswip, reg_addr, i);
		if ((tbar != gsw_mdio_read_tbar(mdio))
			|| (i != gsw_mdio_read(priv, priv->gswip, reg_addr)))
		{
			printk("!RCC: MDIO reg range sweep fail on i=%d", i);
		}
		tbar--;
	}

	// compound test: write 3 regs & read back, with various checks inbetween
	gsw_mdio_write_tbar(mdio, 0);
	reg_addr = GSW_REG_OFFSET_GPIO_PUDSEL; // Write #1
	gsw_mdio_write(priv, priv->gswip, reg_addr, 0x25A5);
	reg_addr_2 = GSW_REG_OFFSET_GPIO2_PUDSEL; // Write #2
	gsw_mdio_write(priv, priv->gswip, reg_addr_2, 0x1A5A);
	tbar = gsw_mdio_read_tbar(mdio);
	if (tbar != ((u32)priv->gswip + reg_addr)) { // expect no tbar change on 2nd write
		printk("!RCC: tbar mismatch: read:0x%x, expected:0x%x", \
			tbar, reg_addr);
		return false;
	}
	reg_addr_3 = GSW_REG_OFFSET_MSPI_DIN45; // Write #3
	gsw_mdio_write(priv, priv->gswip, reg_addr_3, 0xFFFF);
	val = gsw_mdio_read(priv, priv->gswip, reg_addr);
	if (val != 0x25A5) {
		printk("!RCC: read failure: read:0x%x, expected:0x25A5", \
			val);
		return false;
	}
	val = gsw_mdio_read(priv, priv->gswip, reg_addr_2);
	if (val != 0x1A5A) {
		printk("!RCC: read failure: read:0x%x, expected:0x1A5A", \
			val);
		return false;
	}
	val = gsw_mdio_read(priv, priv->gswip, reg_addr_3);
	if (val != 0xFFFF) {
		printk("!RCC: read failure: read:0x%x, expected:0xFFFF", \
			val);
		return false;
	}

	// Verify that we can access the GSW's internal MDIO bus
	// via simple read of FW version from 2 internal PHYs
	val = priv->ds->slave_mii_bus->read(priv->ds->slave_mii_bus, \
						0, MDIO_PHY_REG_FWV);
	if (val != MDIO_PHY_EXPECTED_FWV) {
		printk("!RCC: ERROR rd PHY0 FWV reg: 0x%X", val);
		return false;
	}
	val = priv->ds->slave_mii_bus->read(priv->ds->slave_mii_bus, \
						1, MDIO_PHY_REG_FWV);
	if (val != MDIO_PHY_EXPECTED_FWV) {
		printk("!RCC: ERROR rd PHY1 FWV reg: 0x%X", val);
		return false;
	}

	// compound test for internal MDIO bus:
	// 	perform read-modify-write-read on PHY control register
	val = priv->ds->slave_mii_bus->read(priv->ds->slave_mii_bus, \
						0, MDIO_PHY_REG_LED_CTRL);
	if (val != MDIO_PHY_LED_CTRL_RESET_VAL) {
		printk("!RCC: ERROR w/r PHY0 CTRL reg: read 0x%X", val);
		return false;
	}
	
	val = MDIO_PHY_LED_CTRL_MANUAL_ON;
	err = priv->ds->slave_mii_bus->write(priv->ds->slave_mii_bus, \
						0, MDIO_PHY_REG_LED_CTRL, val);
	if (err) {
		printk("!RCC: ERROR w/r PHY0 CTRL reg: write err:%d", err);
		return false;
	}
	val = priv->ds->slave_mii_bus->read(priv->ds->slave_mii_bus, \
						0, MDIO_PHY_REG_LED_CTRL);
	// check that it stuck
	if (val != MDIO_PHY_LED_CTRL_MANUAL_ON)
	{
		printk("!RCC: ERROR w/r PHY0 CTRL reg: read-back 0x%X", val);
		return false;
	}
	// write original value back
	val = MDIO_PHY_LED_CTRL_RESET_VAL;
	err = priv->ds->slave_mii_bus->write(priv->ds->slave_mii_bus, \
						0, MDIO_PHY_REG_LED_CTRL, val);
	if (err) {
		printk("!RCC: ERROR w/r PHY0 CTRL reg: write-back err:%d", err);
		return false;
	}

	return true;
}
#endif

static int gsw12x_check_port_disable(struct device *dev)
{
	struct device_node *ports, *port;
	u32 reg;

	ports = of_get_child_by_name(dev->of_node, "ports");
	if (!ports) {
		dev_err(dev, "no ports defined in device tree");
		return -ENODEV;
	}
	
	for_each_child_of_node(ports, port) {
		if (of_property_read_u32(port, "reg", &reg))
			break;

		if (((reg == 2) || (reg == 3))
			&& of_device_is_available(port)) {
			dev_err(dev, "ports 2 & 3 must be disabled for MaxLinear GSW12x parts");
			return -EPERM;
		}
	}

	return 0;
}

static int gsw_mdio_probe(struct mdio_device *mdiodev)
{
	struct device *dev = &(mdiodev->dev);
	struct gsw_mdio *mdio_data;
	int err;

	if (of_device_is_compatible(dev->of_node, "maxlinear,gsw12x")) {
		err = gsw12x_check_port_disable(dev);
		if (err)
			return err;
	}

	mdio_data = devm_kzalloc(dev, sizeof(*mdio_data), GFP_KERNEL);
	if (!mdio_data)
		return -ENOMEM;
	
	mdio_data->mdio_dev = mdiodev;
	dev_set_drvdata(dev, mdio_data);

	mdio_data->common.gswip = (void*)GSW_REG_BASE_OFFSET_SWITCH;
	mdio_data->common.mdio = (void*)GSW_REG_BASE_OFFSET_MDIO;
	mdio_data->common.mii = (void*)GSW_REG_BASE_OFFSET_UNUSED;

#if RCC_GSW_ENABLE_BREAKPOINTS
	mdio_data->common.dev = dev;
	gsw_mdio_init_breakpoints(&mdio_data->common);
#endif

#if RCC_GSW_RUN_MDIO_COMM_TESTS
	if (gsw_mdio_comm_tests(&mdio_data->common))
		printk("!RCC: GSW comm test PASS");
	else
		printk("!RCC: GSW comm test FAILURE");
#endif

	err = gsw_core_probe(&mdio_data->common, dev);
	if (err)
		return err;

	/* RCC Edit: create sysfs file to monitor for ESD lockup */
	if (device_create_file(dev, &dev_attr_rcc_phy_esd))
		dev_warn(dev, "unable to create file to monitor for esd lockup\n");

	return 0;
}

static void gsw_mdio_remove(struct mdio_device *pmdiodev)
{
	struct gsw_mdio *mdio_data = mdiodev_get_drvdata(pmdiodev);

	if (!mdio_data)
		return;

	gsw_core_remove(&mdio_data->common);
	mdiodev_set_drvdata(pmdiodev, NULL);
}

static void gsw_mdio_shutdown(struct mdio_device *pmdiodev)
{
	struct gsw_mdio *mdio_data = mdiodev_get_drvdata(pmdiodev);

	if (!mdio_data)
		return;

	gsw_core_shutdown(&mdio_data->common);
	mdiodev_set_drvdata(pmdiodev, NULL);
}

/*-------------------------------------------------------------------------*/

/* Applies to following MaxLinear parts:
 *	GSW140 
 *	GSW120/GSW125
 * Note that for GSW12x parts, switch ports 2 & 3
 * must be marked as disabled in the device tree.
 * 
 * The "EASY GSW140" reference board has it's own string
 * to invoke some exception code in gsw_core_probe().
 * 
 */
static const struct gsw_hw_info gsw_12x_140 = {
	.max_ports 	= 6,
	.cpu_port 	= 5,
	.hw_ops 	= &gsw_mdio_ops,
	.microcode 	= &gswip_pce_microcode_sw2_3,
	.dsa_tag_proto 	= DSA_TAG_PROTO_MAXLINEAR,
};

static const struct of_device_id gsw_mdio_of_match[] = {
	{ .compatible = "maxlinear,gsw12x", 		.data = &gsw_12x_140 },
	{ .compatible = "maxlinear,gsw140", 		.data = &gsw_12x_140 },
	{ .compatible = "maxlinear,gsw140-easy",	.data = &gsw_12x_140 },
	{},
};
MODULE_DEVICE_TABLE(of, gsw_mdio_of_match);

static struct mdio_driver gsw_mdio_driver = {
	.probe = gsw_mdio_probe,
	.remove = gsw_mdio_remove,
	.shutdown = gsw_mdio_shutdown,
	.mdiodrv.driver = {
		.name = "gsw_mdio",
		.of_match_table = of_match_ptr(gsw_mdio_of_match),
	},
};

mdio_module_driver(gsw_mdio_driver);

MODULE_AUTHOR("Harley Sims <hsims@reliablecontrols.com>");
MODULE_DESCRIPTION("MaxLinear / Intel GSW MDIO driver");
MODULE_LICENSE("GPL v2");
