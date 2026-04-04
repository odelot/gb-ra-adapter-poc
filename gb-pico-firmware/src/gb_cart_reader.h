/*
 * GB Cart Reader — Reads a Game Boy cartridge and computes its MD5 hash.
 *
 * Before the console is powered on, the Pico actively drives the cartridge
 * address and control lines to read the full ROM and compute an MD5 hash
 * for RetroAchievements game identification.
 *
 * Pin mapping matches the PIO bus-snooping layout:
 *   GPIO  0‑7  : D0‑D7   (data bus)
 *   GPIO  8‑23 : A0‑A15  (address bus)
 *   GPIO 24    : /WR     (active low — used for MBC bank switching)
 *   GPIO 25    : CLK     (active low during reading)
 *   GPIO 26    : /RD     (active low — directly triggers the ROM output)
 *
 * Reading flow (based on the cartreader project's GB.ino):
 *   1. Read header at 0x0100‑0x014F to get cart type, ROM size, MBC type.
 *   2. Walk every 16 KB bank, streaming 512‑byte chunks through MD5.
 *   3. Return the 32‑char hex MD5 string.
 */

#ifndef GB_CART_READER_H
#define GB_CART_READER_H

#include <stdint.h>
#include <stdbool.h>

/* ---- GPIO pin assignments -------------------------------------------- */

#define GB_D0   0
#define GB_D7   7
#define GB_A0   8
#define GB_A15  23
#define GB_WR   24
#define GB_CLK  25
#define GB_RD   26

/* ---- Cartridge header (parsed from 0x0100‑0x014F) -------------------- */

typedef struct {
    char     title[17];          /* 0x0134‑0x0143 (null‑terminated)     */
    uint8_t  cgb_flag;           /* 0x0143  CGB support                 */
    uint8_t  cart_type;          /* 0x0147  MBC type                    */
    uint8_t  rom_size_code;      /* 0x0148  ROM size code               */
    uint8_t  ram_size_code;      /* 0x0149  RAM size code               */
    uint16_t rom_banks;          /* calculated from rom_size_code       */
    uint8_t  header_checksum;    /* 0x014D                              */
} gb_cart_header_t;

/* ---- Public API ------------------------------------------------------ */

/*
 * Read the full cartridge ROM and compute its MD5 hash.
 *
 * Configures GPIO for active driving, reads the cart header, walks all
 * ROM banks with MBC bank‑switching, streams data through an incremental
 * MD5 computation, then restores GPIO for PIO snooping.
 *
 *   md5_out  — buffer of at least 33 bytes (32 hex chars + NUL)
 *   header_out — optional; if non‑NULL, filled with parsed header info
 *
 * Returns true on success.
 */
bool cart_identify(char *md5_out, gb_cart_header_t *header_out);

#endif /* GB_CART_READER_H */
