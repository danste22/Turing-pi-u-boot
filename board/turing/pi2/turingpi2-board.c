// SPDX-License-Identifier: GPL-2.0+
/*
 * Turing Pi 2 hooks for proper U-Boot (non-SPL).
 *
 * Copyright (C) 2024 Sven Rademakers <sven@turingpi.com>
 */

#include <asm/gpio.h>
#include <bloblist.h>
#include <board_info.h>
#include <common.h>
#include <i2c.h>
#include <linux/delay.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <net.h>
#include <sunxi_gpio.h>

/* Hardware TWI2 is DM / legacy bus 0 on TP2 (see uboot.env). */
#define TURINGPI2_I2C_BUS		0
#define RTL8365MB_I2C_ADDR		0x5c
#define RTL8365MB_PORT_ISOLATION_BASE	0x08a2

static const u16 turingpi2_ethsw_isolation[7] = {
	0x0000, 0x0000, 0x0000, 0x0000, 0x0060, 0x0010, 0x0010,
};

static const char *const turingpi2_emac_compat[] = {
	"allwinner,sun20i-d1-emac",
	"allwinner,sun50i-a64-emac",
	"allwinner,sun50i-h6-emac",
	"allwinner,sun8i-h3-emac",
	"allwinner,sun8i-a83t-emac",
	NULL,
};

#if CONFIG_IS_ENABLED(BLOBLIST)
static u16 turingpi2_hw_version(void)
{
	tpi_board_info *info;
	int ret;

	ret = bloblist_maybe_init();
	if (ret)
		return TP_VER(2, 4, 0);

	info = bloblist_find(BLOBLISTT_U_BOOT_SPL_HANDOFF, 0);
	if (!info)
		return TP_VER(2, 4, 0);

	return info->hw_version;
}

static bool turingpi2_mac_from_bloblist(u8 *mac)
{
	tpi_board_info *info;
	int ret;

	ret = bloblist_maybe_init();
	if (ret)
		return false;

	info = bloblist_find(BLOBLISTT_U_BOOT_SPL_HANDOFF, 0);
	if (!info)
		return false;

	memcpy(mac, info->mac, ARP_HLEN);
	if (!is_valid_ethaddr(mac))
		return false;

	if (compute_crc(info) != info->crc32)
		printf("BMC MAC: SPL handoff CRC mismatch\n");

	return true;
}
#else
static u16 turingpi2_hw_version(void)
{
	return TP_VER(2, 4, 0);
}
#endif

static bool turingpi2_mac_read(u8 *mac)
{
#if CONFIG_IS_ENABLED(BLOBLIST)
	return turingpi2_mac_from_bloblist(mac);
#else
	return false;
#endif
}

static void turingpi2_ethsw_reset_release(u16 hw_version)
{
	unsigned int pin = (hw_version < TP_VER(2, 5, 0)) ?
			   SUNXI_GPG(13) : SUNXI_GPG(3);

	/* SPL asserts active-low reset; float the line to deassert. */
	gpio_direction_input(pin);
	mdelay(150);
}

#if CONFIG_IS_ENABLED(DM_I2C)
static int turingpi2_rtk_reg_write(u16 reg, u16 val)
{
	struct udevice *dev;
	u8 data[2];
	int ret;

	ret = i2c_get_chip_for_busnum(TURINGPI2_I2C_BUS, RTL8365MB_I2C_ADDR,
				      2, &dev);
	if (ret)
		return ret;

	data[0] = val & 0xff;
	data[1] = val >> 8;

	return dm_i2c_write(dev, reg, data, sizeof(data));
}
#endif

int turingpi2_ethsw_isolate(void)
{
	u16 hw_version = turingpi2_hw_version();
	int port, ret, failures = 0;

	turingpi2_ethsw_reset_release(hw_version);

#if !CONFIG_IS_ENABLED(DM_I2C)
	return 0;
#else
	for (port = 0; port < ARRAY_SIZE(turingpi2_ethsw_isolation); port++) {
		ret = turingpi2_rtk_reg_write(
			RTL8365MB_PORT_ISOLATION_BASE + port,
			turingpi2_ethsw_isolation[port]);
		if (ret)
			failures++;
	}

	if (failures)
		printf("BMC ethsw: %d isolation write(s) failed\n", failures);

	return 0;
#endif
}

static void turingpi2_emac_set_fdt_mac(void *fdt, const u8 *mac)
{
	int node = -1, i;

	if (!fdt)
		return;

	for (i = 0; turingpi2_emac_compat[i]; i++) {
		node = fdt_node_offset_by_compatible(fdt, 0,
						     turingpi2_emac_compat[i]);
		if (node >= 0)
			break;
	}
	if (node < 0)
		return;

	fdt_setprop(fdt, node, "local-mac-address", mac, ARP_HLEN);
}

int turingpi2_mac_apply_late(void)
{
	u8 mac[6];

	if (!turingpi2_mac_read(mac)) {
		printf("BMC MAC: factory address not found\n");
		return 0;
	}

	printf("BMC MAC %pM\n", mac);
	return 0;
}

int turingpi2_mac_apply_fdt(void *fdt)
{
	u8 mac[6];

	if (!fdt || !turingpi2_mac_read(mac))
		return 0;

	turingpi2_emac_set_fdt_mac(fdt, mac);
	return 0;
}
