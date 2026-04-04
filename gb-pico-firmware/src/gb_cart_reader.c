/*
 * GB Cart Reader — Implementation
 *
 * Reads a Game Boy cartridge directly from the Pico (console is OFF).
 * Based on the cartreader project's GB.ino read routines.
 */

#include "gb_cart_reader.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

/* MD5 from rcheevos (already compiled into the binary) */
#include "rhash/md5.h"

/* ================================================================
 *  Internal helpers
 * ================================================================ */

/* 512-byte chunk buffer for streaming into MD5 */
static uint8_t chunk_buf[512];

/* ---- GPIO configuration --------------------------------------------- */

static void cart_gpio_init_for_reading(void)
{
    /* Data bus D0‑D7 (GPIO 0‑7): INPUT with pull‑down */
    for (int i = GB_D0; i <= GB_D7; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_IN);
        gpio_pull_down(i);
    }

    /* Address bus A0‑A15 (GPIO 8‑23): OUTPUT, drive strength 12 mA */
    for (int i = GB_A0; i <= GB_A15; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_OUT);
        gpio_set_drive_strength(i, GPIO_DRIVE_STRENGTH_12MA);
        gpio_put(i, 0);
    }

    /* /WR (GPIO 24): OUTPUT, idle HIGH (inactive) */
    gpio_init(GB_WR);
    gpio_set_dir(GB_WR, GPIO_OUT);
    gpio_put(GB_WR, 1);

    /* CLK (GPIO 25): OUTPUT, held LOW during reading */
    gpio_init(GB_CLK);
    gpio_set_dir(GB_CLK, GPIO_OUT);
    gpio_put(GB_CLK, 0);

    /* /RD (GPIO 26): OUTPUT, idle HIGH (inactive) */
    gpio_init(GB_RD);
    gpio_set_dir(GB_RD, GPIO_OUT);
    gpio_put(GB_RD, 1);
}

static void cart_gpio_restore_for_pio(void)
{
    /* Restore all 28 PIO-observed pins (GPIO 0‑27) as inputs.
     * Core 1's setupPIO() will reconfigure them later. */
    for (int i = 0; i < 28; i++) {
        gpio_init(i);
        gpio_set_dir(i, GPIO_IN);
        gpio_pull_down(i);
    }
}

/* ---- Low-level bus operations --------------------------------------- */

/*
 * Set address on A0‑A15 using a single masked GPIO write.
 * A0 = GPIO 8, A15 = GPIO 23  →  shift address left by 8.
 */
static inline void set_address(uint16_t addr)
{
    uint32_t mask  = 0x00FFFF00u;           /* GPIO 8‑23 */
    uint32_t value = ((uint32_t)addr) << 8;
    gpio_put_masked(mask, value);
}

/*
 * Short delay (~200 ns at 250 MHz).
 * Game Boy ROM chips typically need 70‑150 ns access time.
 * 50 NOPs × 4 ns = 200 ns — comfortably above the spec.
 */
#define NOP10() __asm volatile( \
    "nop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\nnop\n")
#define CART_DELAY() do { NOP10(); NOP10(); NOP10(); NOP10(); NOP10(); } while(0)

/*
 * Read one byte from the cartridge at `addr`.
 * Protocol (mirrors cartreader's readByte_GB):
 *   1. Put address on bus
 *   2. Wait for address to settle
 *   3. Pull /RD LOW (active)
 *   4. Wait for data to stabilise
 *   5. Read D0‑D7
 *   6. Release /RD HIGH
 */
static uint8_t cart_read_byte(uint16_t addr)
{
    set_address(addr);
    CART_DELAY();

    gpio_put(GB_RD, 0);            /* /RD active  */
    CART_DELAY();

    uint8_t data = (uint8_t)(gpio_get_all() & 0xFFu);  /* D0‑D7 = GPIO 0‑7 */

    gpio_put(GB_RD, 1);            /* /RD release */
    CART_DELAY();

    return data;
}

/*
 * Write one byte to the cartridge at `addr` (used for MBC bank switching).
 * Protocol (mirrors cartreader's writeByte_GB):
 *   1. Put address on bus
 *   2. Set data bus to OUTPUT, put data
 *   3. Pulse /WR LOW then HIGH
 *   4. Restore data bus to INPUT
 */
static void cart_write_byte(uint16_t addr, uint8_t data)
{
    set_address(addr);

    /* D0‑D7 → OUTPUT */
    for (int i = GB_D0; i <= GB_D7; i++)
        gpio_set_dir(i, GPIO_OUT);

    /* Put data on D0‑D7 in a single write */
    gpio_put_masked(0xFFu, (uint32_t)data);

    CART_DELAY();
    gpio_put(GB_WR, 0);            /* /WR active  */
    CART_DELAY();
    gpio_put(GB_WR, 1);            /* /WR release */
    CART_DELAY();

    /* D0‑D7 → INPUT (with pull‑down) */
    for (int i = GB_D0; i <= GB_D7; i++) {
        gpio_set_dir(i, GPIO_IN);
        gpio_pull_down(i);
    }
}

/* ---- Header parsing ------------------------------------------------- */

static bool cart_read_header(gb_cart_header_t *hdr)
{
    uint8_t raw[0x50];  /* 0x0100 – 0x014F */

    for (int i = 0; i < 0x50; i++)
        raw[i] = cart_read_byte(0x0100 + i);

    /* Header checksum: ∑(0x0134..0x014C), subtract‑and‑minus‑one per byte */
    uint8_t cksum = 0;
    for (int i = 0x34; i < 0x4D; i++)      /* offsets relative to raw[0]=0x0100 */
        cksum = cksum - raw[i] - 1;

    if (cksum != raw[0x4D]) {
        /* Retry once — sometimes the first few reads after power‑on glitch. */
        printf("CART: header checksum mismatch (got 0x%02X, expected 0x%02X), retrying\n",
               cksum, raw[0x4D]);
        for (int i = 0; i < 0x50; i++)
            raw[i] = cart_read_byte(0x0100 + i);

        cksum = 0;
        for (int i = 0x34; i < 0x4D; i++)
            cksum = cksum - raw[i] - 1;

        if (cksum != raw[0x4D]) {
            printf("CART: header checksum FAILED (0x%02X != 0x%02X)\n", cksum, raw[0x4D]);
            return false;
        }
    }

    /* Title: up to 16 bytes at 0x0134.  CGB‑aware titles are 15 bytes. */
    int title_len = (raw[0x43] == 0x80 || raw[0x43] == 0xC0) ? 15 : 16;
    memcpy(hdr->title, &raw[0x34], title_len);
    hdr->title[title_len] = '\0';

    hdr->cgb_flag       = raw[0x43];
    hdr->cart_type       = raw[0x47];
    hdr->rom_size_code   = raw[0x48];
    hdr->ram_size_code   = raw[0x49];
    hdr->header_checksum = raw[0x4D];

    /* ROM banks: 2 << rom_size_code  (32 KB minimum = 2 banks of 16 KB) */
    if (hdr->rom_size_code <= 0x08)
        hdr->rom_banks = 2u << hdr->rom_size_code;
    else
        hdr->rom_banks = 2;  /* unknown / fallback */

    printf("CART: title='%s'  type=0x%02X  rom_banks=%u  rom_size_code=0x%02X\n",
           hdr->title, hdr->cart_type, hdr->rom_banks, hdr->rom_size_code);

    return true;
}

/* ---- MBC bank switching --------------------------------------------- */

/*
 * Select the given ROM bank in the 0x4000‑0x7FFF window.
 * Mirrors the cartreader logic for each MBC family.
 */
static void set_rom_bank(uint8_t cart_type, uint16_t bank)
{
    switch (cart_type) {

    /* ---- No MBC (ROM only) ---- */
    case 0x00:
    case 0x08:
    case 0x09:
        break;

    /* ---- MBC1 ---- */
    case 0x01:
    case 0x02:
    case 0x03:
        cart_write_byte(0x6000, 0x00);              /* ROM banking mode  */
        cart_write_byte(0x4000, (bank >> 5) & 0x03);/* upper 2 bits      */
        cart_write_byte(0x2000, bank & 0x1F);       /* lower 5 bits      */
        if ((bank & 0x1F) == 0) {
            /* MBC1 quirk: bank 0 maps to bank 1 */
            cart_write_byte(0x2000, 0x01);
        }
        break;

    /* ---- MBC2 ---- */
    case 0x05:
    case 0x06:
        cart_write_byte(0x2100, bank & 0x0F);
        break;

    /* ---- MBC3 ---- */
    case 0x0F:
    case 0x10:
    case 0x11:
    case 0x12:
    case 0x13:
        cart_write_byte(0x2100, bank & 0x7F);
        if ((bank & 0x7F) == 0)
            cart_write_byte(0x2100, 0x01);
        break;

    /* ---- MBC5 ---- */
    case 0x19:
    case 0x1A:
    case 0x1B:
    case 0x1C:
    case 0x1D:
    case 0x1E:
        cart_write_byte(0x2000, bank & 0xFF);
        cart_write_byte(0x3000, (bank >> 8) & 0x01);
        break;

    /* ---- MBC6 (half-banks, rare) ---- */
    case 0x20:
        cart_write_byte(0x2000, bank & 0xFF);
        cart_write_byte(0x3000, bank & 0xFF);
        break;

    /* ---- Fallback: try MBC5-style (covers most HuC / unusual types) ---- */
    default:
        cart_write_byte(0x2000, bank & 0xFF);
        cart_write_byte(0x3000, (bank >> 8) & 0x01);
        break;
    }
}

/* ================================================================
 *  Public: cart_identify
 * ================================================================ */

bool cart_identify(char *md5_out, gb_cart_header_t *header_out)
{
    gb_cart_header_t hdr;
    md5_state_t md5_state;

    printf("CART: starting cartridge identification\n");

    cart_gpio_init_for_reading();

    if (!cart_read_header(&hdr)) {
        cart_gpio_restore_for_pio();
        return false;
    }

    if (header_out)
        *header_out = hdr;

    md5_init(&md5_state);

    /* ----- Bank 0: 0x0000 – 0x3FFF (always mapped) ----- */
    printf("CART: reading bank 0/%-u\n", hdr.rom_banks);
    for (uint16_t addr = 0x0000; addr < 0x4000; addr += 512) {
        for (int i = 0; i < 512; i++)
            chunk_buf[i] = cart_read_byte(addr + (uint16_t)i);
        md5_append(&md5_state, chunk_buf, 512);
    }

    /* ----- Banks 1 .. N-1: 0x4000 – 0x7FFF (bank‑switched) ----- */
    for (uint16_t bank = 1; bank < hdr.rom_banks; bank++) {
        if ((bank & 0x0F) == 0)
            printf("CART: reading bank %u/%u\n", bank, hdr.rom_banks);

        set_rom_bank(hdr.cart_type, bank);

        for (uint16_t addr = 0x4000; addr < 0x8000; addr += 512) {
            for (int i = 0; i < 512; i++)
                chunk_buf[i] = cart_read_byte(addr + (uint16_t)i);
            md5_append(&md5_state, chunk_buf, 512);
        }
    }

    /* ----- Finalise MD5 ----- */
    md5_byte_t digest[16];
    md5_finish(&md5_state, digest);

    for (int i = 0; i < 16; i++)
        sprintf(md5_out + i * 2, "%02x", digest[i]);
    md5_out[32] = '\0';

    printf("CART: identification complete — MD5=%s\n", md5_out);

    cart_gpio_restore_for_pio();
    return true;
}
