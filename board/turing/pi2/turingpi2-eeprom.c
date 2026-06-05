// SPDX-License-Identifier: GPL-2.0+
/*
 * Turing Pi 2 board EEPROM access (SPL direct TWI2 and proper-U-Boot DM).
 *
 * Copyright (C) 2024 Sven Rademakers <sven@turingpi.com>
 */

#include <board_info.h>
#include <dm.h>
#include <errno.h>
#include <i2c.h>
#include <linux/delay.h>

#define TPI_EEPROM_ADDR	0x50
#define TPI_EEPROM_RETRIES	3

#if defined(CONFIG_XPL_BUILD) && defined(CONFIG_ARCH_SUNXI)
int sunxi_mvtwsi_early_read(u8 chip, unsigned int addr, int alen,
			    u8 *buffer, int len);
#endif

static int turingpi2_eeprom_read_once(u8 *buf, int len)
{
#if defined(CONFIG_XPL_BUILD) && defined(CONFIG_ARCH_SUNXI)
	return sunxi_mvtwsi_early_read(TPI_EEPROM_ADDR, 0, 1, buf, len);
#elif CONFIG_IS_ENABLED(DM_I2C)
	struct udevice *dev;
	int ret;

	ret = i2c_get_chip_for_busnum(0, TPI_EEPROM_ADDR, 1, &dev);
	if (ret)
		return ret;

	return dm_i2c_read(dev, 0, buf, len);
#else
	return -ENOSYS;
#endif
}

int turingpi2_eeprom_read(u8 *buf, int len)
{
	int ret = -EIO;
	int attempt;

	if (!buf || len <= 0)
		return -EINVAL;

	for (attempt = 0; attempt < TPI_EEPROM_RETRIES; attempt++) {
		if (attempt)
			mdelay(5);

		unblock_twi2_bus();
		udelay(500);

		ret = turingpi2_eeprom_read_once(buf, len);
		if (!ret)
			return 0;
	}

	return ret;
}

bool turingpi2_board_info_valid(tpi_board_info *info, int read_err)
{
	if (read_err || !info)
		return false;

	return compute_crc(info) == info->crc32;
}

int turingpi2_board_info_read(tpi_board_info *info)
{
	int ret;

	if (!info)
		return -EINVAL;

	ret = turingpi2_eeprom_read((u8 *)info, sizeof(*info));
	if (ret)
		return ret;

	if (!turingpi2_board_info_valid(info, 0))
		return -EINVAL;

	return 0;
}
