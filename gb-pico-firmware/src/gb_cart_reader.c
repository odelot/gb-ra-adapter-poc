/*
 * GB Cart Reader — Implementation
 *
 * Reads a Game Boy cartridge directly from the Pico (console is OFF).
 * Reads the first 512 bytes at 0x0000, computes CRC32, and returns it
 * as an 8-char hex string for lookup on the ESP32 side.
 */

#include "gb_cart_reader.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"

/* ================================================================
 *  Internal helpers
 * ================================================================ */

/* Standard CRC32 (ISO 3309 / PKZIP polynomial 0xEDB88320) */
static uint32_t crc32_compute(const uint8_t *data, int len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* 512-byte sample from 0x0000 */
static uint8_t sample_buf[512];

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

    return true;
}

/* ================================================================
 *  Public: cart_identify
 * ================================================================ */

/*
 * Read the first 512 bytes at 0x0000, compute CRC32, and return it
 * as an 8-char hex string in crc32_out (buffer must be >= 9 bytes).
 */
bool cart_identify(char *crc32_out, gb_cart_header_t *header_out)
{
    gb_cart_header_t hdr;

    printf("CART: starting cartridge identification\n");

    cart_gpio_init_for_reading();

    if (!cart_read_header(&hdr)) {
        cart_gpio_restore_for_pio();
        return false;
    }

    if (header_out)
        *header_out = hdr;

    /* ----- Sample first 512 bytes at 0x0000 ----- */
    for (int i = 0; i < 512; i++)
        sample_buf[i] = cart_read_byte((uint16_t)i);

    /* ----- CRC32 of 512 bytes ----- */
    uint32_t crc = crc32_compute(sample_buf, 512);
    sprintf(crc32_out, "%08lx", (unsigned long)crc);

    cart_gpio_restore_for_pio();

    /* Diagnostics — only printed AFTER GPIO restored */
    printf("CART: title='%s'  type=0x%02X  rom_banks=%u\n",
           hdr.title, hdr.cart_type, hdr.rom_banks);
    printf("CART: CRC32=%s\n", crc32_out);

    return true;
}
