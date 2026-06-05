// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2023 Sam Edwards <CFSworks@gmail.com>
 * Copyright (C) 2024 Sven Rademakers <sven@turingpi.com>
 *
 * Early init for the Turing Pi 2 clusterboard.
 */

#include <asm/gpio.h>
#include <asm/io.h>
#include <bloblist.h>
#include <board_info.h>
#include <boot_fit.h>
#include <image.h>
#include <init.h>
#include <linux/delay.h>
#include <linux/libfdt.h>
#include <linux/string.h>
#include <mapmem.h>
#include <spl.h>
#include <sunxi_gpio.h>
#include <u-boot/crc.h>

u32 compute_crc(const tpi_board_info *info)
{
	int info_offset = offsetof(tpi_board_info, hdr_version);
	u32 crc = crc32(0, (void *)info + info_offset,
			sizeof(tpi_board_info) - info_offset);

	return ((crc & 0x000000FF) << 24) | ((crc & 0x0000FF00) << 8) |
	       ((crc & 0x00FF0000) >> 8) | ((crc & 0xFF000000) >> 24);
}

#ifdef CONFIG_XPL_BUILD

#define TURING_PI2_LATCH_STATE_ADDR 0x0709010c
#define TURING_PI2_BOOT_COOKIE_ADDR 0x07090108
#define TURING_PI2_BOOT_COOKIE_WARM 0x32695054 /* "TPi2" ASCII */
#define TURING_PI2_BOOT_COOKIE_FEL 0x5aa5a55a

static void turingpi2_ethsw_rst(u16 tpi_version)
{
	if (tpi_version < TP_VER(2, 5, 0))
		gpio_direction_output(SUNXI_GPG(13), 0);
	else
		gpio_direction_output(SUNXI_GPG(3), 0);
}

static void init_latches(u16 tpi_version)
{
	if (tpi_version >= TP_VER(2, 5, 0)) {
		gpio_direction_output(SUNXI_GPD(7), 0);
		gpio_direction_output(SUNXI_GPD(6), 0);
		gpio_direction_output(SUNXI_GPD(5), 0);
		gpio_direction_output(SUNXI_GPD(3), 0);
		gpio_direction_output(SUNXI_GPD(4), 0);
		gpio_direction_output(SUNXI_GPD(8), 0);
		gpio_direction_output(SUNXI_GPD(9), 0);
		gpio_direction_output(SUNXI_GPD(10), 0);
		gpio_direction_output(SUNXI_GPD(11), 0);
		gpio_direction_output(SUNXI_GPD(20), 1);
		udelay(50);

		writel(readl(TURING_PI2_LATCH_STATE_ADDR) & 0xFFFFFE00,
		       TURING_PI2_LATCH_STATE_ADDR);
	}
}

static int board_info_from_eeprom(tpi_board_info *info)
{
	return turingpi2_board_info_read(info);
}

#if CONFIG_IS_ENABLED(BLOBLIST)
static tpi_board_info *setup_bloblist(void)
{
	int init_res = bloblist_init();
	void *board_tag;

	if (init_res) {
		printf("bloblist init err 0x%x\n", init_res);
		return NULL;
	}

	board_tag = bloblist_add(BLOBLISTT_U_BOOT_SPL_HANDOFF,
				 sizeof(tpi_board_info), 0);
	if (!board_tag)
		printf("no space for board_info in bloblist\n");

	return board_tag;
}
#endif

int tp_board_init(void)
{
	u32 cookie = readl(TURING_PI2_BOOT_COOKIE_ADDR);
	tpi_board_info *info = NULL;
	u16 version = TP_VER(2, 4, 0);
	int result = -ENOENT;

	if (cookie == TURING_PI2_BOOT_COOKIE_FEL) {
		writel(TURING_PI2_BOOT_COOKIE_WARM, TURING_PI2_BOOT_COOKIE_ADDR);
		return 1;
	}

#if CONFIG_IS_ENABLED(BLOBLIST)
	info = setup_bloblist();
	if (!info)
		return 0;
#else
	tpi_board_info stack;

	info = &stack;
#endif

	if (info)
		result = board_info_from_eeprom(info);

	if (result)
		printf("Error: reading EEPROM %d\n", result);

	if (result || compute_crc(info) != info->crc32) {
		printf("Error(%x): invalid board info, defaulting to version 0x%x. crc=%x expected=%x ver=%x\n",
		       result, version, info->crc32, compute_crc(info),
		       info->hw_version);
		info->hw_version = version;
	} else {
		version = info->hw_version;
	}

	if (cookie != TURING_PI2_BOOT_COOKIE_WARM) {
		turingpi2_ethsw_rst(version);
		init_latches(version);
		writel(TURING_PI2_BOOT_COOKIE_WARM, TURING_PI2_BOOT_COOKIE_ADDR);
	}

#if CONFIG_IS_ENABLED(BLOBLIST)
	bloblist_finish();
#endif
	return 0;
}

#ifdef CONFIG_TARGET_TURINGPI2
#if CONFIG_IS_ENABLED(OF_SEPARATE) && CONFIG_IS_ENABLED(BLOBLIST) && \
	CONFIG_IS_ENABLED(MULTI_DTB_FIT)
static bool turingpi2_fdt_valid(const void *blob)
{
	int totalsize;

	if (!blob || fdt_magic(blob) != FDT_MAGIC)
		return false;

	totalsize = fdt_totalsize(blob);
	return totalsize >= (int)sizeof(struct fdt_header) &&
	       totalsize <= (1024 * 1024);
}

static int turingpi2_stash_fdt(const void *blob)
{
	int size = fdt_totalsize(blob);
	void *stash;
	int ret;

	if (!turingpi2_fdt_valid(blob))
		return -EINVAL;

	memcpy(map_sysmem(TPI2_SPL_FDT_ADDR, size), blob, size);

	ret = bloblist_maybe_init();
	if (ret)
		return ret;

	if (bloblist_find(BLOBLISTT_CONTROL_FDT, 0))
		return 0;

	stash = bloblist_add(BLOBLISTT_CONTROL_FDT, size, 0);
	if (!stash)
		return -ENOSPC;

	memcpy(stash, blob, size);
	return 0;
}

static void *turingpi2_find_multidtb_fit(ulong base, ulong size)
{
	u8 *start, *p;
	ulong scan_len = size;

	if (size < sizeof(struct fdt_header))
		return NULL;

	start = map_sysmem(base, scan_len);
	for (p = start + scan_len - 4; p >= start; p -= 4) {
		ulong remain = scan_len - (p - start);

		if (fdt_magic(p) != FDT_MAGIC)
			continue;
		if (fdt_totalsize(p) > remain ||
		    fdt_totalsize(p) < sizeof(struct fdt_header))
			continue;
		if (fit_check_format(p, IMAGE_SIZE_INVAL))
			continue;
		if (fdt_path_offset(p, "/images") < 0)
			continue;
		return p;
	}

	return NULL;
}

void spl_perform_board_fixups(struct spl_image_info *spl_image)
{
	void *fit, *dtb;

	if (!spl_image)
		return;

	dtb = spl_image_fdt_addr(spl_image);
	if (turingpi2_fdt_valid(dtb)) {
		turingpi2_stash_fdt(dtb);
		return;
	}

	fit = turingpi2_find_multidtb_fit(spl_image->load_addr, spl_image->size);
	if (!fit)
		return;

	dtb = locate_dtb_in_fit(fit);
	if (turingpi2_fdt_valid(dtb))
		turingpi2_stash_fdt(dtb);
}
#endif /* OF_SEPARATE && BLOBLIST && MULTI_DTB_FIT */
#endif /* CONFIG_TARGET_TURINGPI2 */

#endif /* CONFIG_XPL_BUILD */

#if defined(CONFIG_TARGET_TURINGPI2) && CONFIG_IS_ENABLED(OF_CONTROL)
int board_fit_config_name_match(const char *name)
{
#if CONFIG_IS_ENABLED(BLOBLIST)
	tpi_board_info *info;
	int init_res = bloblist_maybe_init();

	if (init_res)
		return -ENODATA;

	info = bloblist_find(BLOBLISTT_U_BOOT_SPL_HANDOFF, 0);
	if (!info)
		return -ENODATA;

	if (info->hw_version == TP_VER(2, 4, 0) && strstr(name, "-v2.4"))
		return 0;
	if (info->hw_version >= TP_VER(2, 5, 0) && strstr(name, "-v2.5"))
		return 0;
#endif
	return -EINVAL;
}
#endif
