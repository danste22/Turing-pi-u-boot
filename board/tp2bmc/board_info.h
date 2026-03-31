#ifndef BOARD_INFO_H
#define BOARD_INFO_H

#include <linux/types.h>

#define TP_VER(maj, min, pat) \
	((maj << 11) | ((min & 0x1F) << 6) | (pat & 0x3F))

#pragma pack(push, 1)
typedef struct tpi_board_info {
	/* Keep first two bytes reserved for Realtek switch unmanaged mode. */
	uint16_t reserved;
	/* CRC is calculated over this struct starting after this field. */
	uint32_t crc32;
	uint16_t hdr_version;
	uint16_t hw_version;
	/* Days since May 1st 2024 (factory flash date). */
	uint16_t factory_date;
	char factory_serial[16];
	char product_name[16];
	char mac[6];
} tpi_board_info;
#pragma pack(pop)

u32 compute_crc(tpi_board_info *info);

#endif /* BOARD_INFO_H */
