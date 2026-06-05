// SPDX-License-Identifier: GPL-2.0+
/*
 * Turing Pi 2 hooks for proper U-Boot (non-SPL).
 *
 * Copyright (C) 2024 Sven Rademakers <sven@turingpi.com>
 */

#include <asm/gpio.h>
#include <asm-generic/gpio.h>
#include <bloblist.h>
#include <board_info.h>
#include <boot_fit.h>
#include <dm.h>
#include <env.h>
#include <image.h>
#include <i2c.h>
#include <asm-generic/global_data.h>
#include <asm/sections.h>
#include <serial.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/delay.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <mapmem.h>
#include <stdbool.h>
#include <net.h>
#include <sunxi_gpio.h>

DECLARE_GLOBAL_DATA_PTR;

/* RTL8370MB on hardware TWI2 (PE12/PE13), sole enabled I2C bus (seq 0). */
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
static tpi_board_info cached_info;
static bool cached_info_ready;

static const tpi_board_info *turingpi2_board_info_get(void)
{
	tpi_board_info *info;
	int ret;

	if (cached_info_ready)
		return &cached_info;

	ret = bloblist_maybe_init();
	if (!ret) {
		info = bloblist_find(BLOBLISTT_U_BOOT_SPL_HANDOFF, 0);
		if (turingpi2_board_info_valid(info, info ? 0 : -ENOENT)) {
			cached_info = *info;
			cached_info_ready = true;
			return &cached_info;
		}
	}

	if (!turingpi2_board_info_read(&cached_info)) {
		cached_info_ready = true;
		return &cached_info;
	}

	return NULL;
}

static u16 turingpi2_hw_version(void)
{
	const tpi_board_info *info = turingpi2_board_info_get();

	if (!info)
		return TP_VER(2, 4, 0);

	return info->hw_version;
}

static bool turingpi2_mac_from_bloblist(u8 *mac)
{
	const tpi_board_info *info = turingpi2_board_info_get();

	if (!info)
		return false;

	memcpy(mac, info->mac, ARP_HLEN);
	if (!is_valid_ethaddr(mac))
		return false;

	if (compute_crc(info) != info->crc32)
		printf("BMC MAC: board info CRC mismatch\n");

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
	const char *gpio_name = (hw_version < TP_VER(2, 5, 0)) ? "PG13" : "PG3";
	struct gpio_desc reset_gpio;
	int ret;

	/* SPL asserts active-low reset; float the line to deassert. */
	ret = dm_gpio_lookup_name(gpio_name, &reset_gpio);
	if (ret)
		goto fallback;

	ret = dm_gpio_request(&reset_gpio, "ethsw-reset");
	if (ret)
		goto fallback;

	dm_gpio_set_dir_flags(&reset_gpio, GPIOD_IS_IN);
	dm_gpio_free(reset_gpio.dev, &reset_gpio);
	mdelay(150);
	return;

fallback:
	/* Pre-DM fallback for boards without gpio lookup names. */
	gpio_direction_input((hw_version < TP_VER(2, 5, 0)) ?
			     SUNXI_GPG(13) : SUNXI_GPG(3));
	mdelay(150);
}

#if CONFIG_IS_ENABLED(DM_I2C)
static int turingpi2_rtk_chip(struct udevice **devp)
{
	struct udevice *bus;
	int ret;

	ret = uclass_get_device_by_seq(UCLASS_I2C, TURINGPI2_I2C_BUS, &bus);
	if (ret)
		return ret;

	/*
	 * RTL8365MB SMI does not ACK a normal probe transaction; bind the
	 * chip from the DT without i2c_probe_chip().
	 */
	return i2c_get_chip(bus, RTL8365MB_I2C_ADDR, 1, devp);
}

static int turingpi2_rtk_reg_write(struct udevice *dev, u16 reg, u16 val)
{
	struct i2c_msg msg;
	u8 buf[4];

	/* Match Linux realtek-smi-i2c: one write, reg/le16 + val/le16. */
	buf[0] = reg & 0xff;
	buf[1] = reg >> 8;
	buf[2] = val & 0xff;
	buf[3] = val >> 8;

	msg.addr = RTL8365MB_I2C_ADDR;
	msg.flags = 0;
	msg.len = sizeof(buf);
	msg.buf = buf;

	return dm_i2c_xfer(dev, &msg, 1);
}
#endif

int turingpi2_ethsw_isolate(void)
{
	u16 hw_version = turingpi2_hw_version();
	struct udevice *rtk;
	int port, ret, failures = 0;

	turingpi2_ethsw_reset_release(hw_version);

#if !CONFIG_IS_ENABLED(DM_I2C)
	return 0;
#else
	ret = turingpi2_rtk_chip(&rtk);
	if (ret) {
		printf("BMC ethsw: switch not found (err=%d)\n", ret);
		return 0;
	}

	for (port = 0; port < ARRAY_SIZE(turingpi2_ethsw_isolation); port++) {
		ret = turingpi2_rtk_reg_write(
			rtk,
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

void turingpi2_set_fit_config_env(void)
{
	u16 hw = turingpi2_hw_version();
	const char *fit_config;
	char ver[12];

	if (hw >= TP_VER(2, 5, 2))
		fit_config = "config-v2.5.2";
	else if (hw >= TP_VER(2, 5, 1))
		fit_config = "config-v2.5.1";
	else if (hw >= TP_VER(2, 5, 0))
		fit_config = "config-v2.5.0";
	else
		fit_config = "config-v2.4.0";

	snprintf(ver, sizeof(ver), "v%d.%d.%d", hw >> 11,
		 (hw >> 6) & 0x1F, hw & 0x3F);
	env_set("tpi_fit_config", fit_config);
	printf("FIT config %s (EEPROM %s)\n", fit_config, ver);
}

#if CONFIG_IS_ENABLED(OF_SEPARATE) && CONFIG_IS_ENABLED(OF_BOARD) && \
	CONFIG_IS_ENABLED(MULTI_DTB_FIT)
int board_fdt_blob_setup(void **fdtp)
{
	void *stash = map_sysmem(TPI2_SPL_FDT_ADDR, 0);
	void *fit, *dtb;

	if (!fdt_check_header(stash)) {
		*fdtp = stash;
		return 0;
	}

	if (!fit_check_format(stash, IMAGE_SIZE_INVAL)) {
		dtb = locate_dtb_in_fit(stash);
		if (dtb) {
			*fdtp = dtb;
			return 0;
		}
	}

	fit = (void *)_end;
	if (!fit_check_format(fit, IMAGE_SIZE_INVAL)) {
		dtb = locate_dtb_in_fit(fit);
		if (dtb) {
			*fdtp = dtb;
			return 0;
		}
	}

	return -EEXIST;
}
#endif
