#ifndef BOARD_INFO_H
#define BOARD_INFO_H

#include <linux/types.h>
#include <stdbool.h>

#define TP_VER(maj, min, pat)\
	((maj << 11) | ((min & 0x1F) << 6) | (pat & 0x3F))

/* SPL copies the chosen DTB here for U-Boot proper (see board_fdt_blob_setup). */
#define TPI2_SPL_FDT_ADDR 0x41800000UL

#pragma pack(push, 1)
typedef struct tpi_board_info {
	uint16_t reserved;
	uint32_t crc32;
	uint16_t hdr_version;
	uint16_t hw_version;
	uint16_t factory_date;
	char factory_serial[16];
	char product_name[16];
	char mac[6];
} tpi_board_info;
#pragma pack(pop)

u32 compute_crc(const tpi_board_info *info);

void unblock_twi2_bus(void);
int turingpi2_eeprom_read(u8 *buf, int len);
bool turingpi2_board_info_valid(tpi_board_info *info, int read_err);
int turingpi2_board_info_read(tpi_board_info *info);

#endif /* BOARD_INFO_H */
