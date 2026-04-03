/*
 * GB CPU Emulator Core — adapted from GBInterceptor cpubus.c
 *
 * Follows the real Game Boy CPU by observing bus traffic via PIO.
 * Tracks all register changes, memory writes (including HRAM),
 * interrupt handling and DMA skipping.
 *
 * PPU is reduced to a minimal timing model (Y counter) so that
 * IO‑register reads (LY, STAT, DIV) return plausible values.
 *
 * Based on GBInterceptor by Sebastian Staacks
 * https://github.com/Staacks/GBInterceptor
 */

#include "gb_cpu_emu.h"
#include "gb_opcodes.h"

#include <string.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "pico/platform.h"
#include "pico/time.h"
#include "pico/mutex.h"
#include "hardware/pio.h"
#include "memory-bus.pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/structs/systick.h"

/* ================================================================
 *  Cycle‑ratio calibration (same scheme as GBInterceptor)
 * ================================================================ */

uint32_t cycleRatio;
#define CYCLE_RATIO_STATISTIC_SKIP 250
#define CYCLE_RATIO_STATISTIC_SIZE 1000

/* ================================================================
 *  PIO helpers
 * ================================================================ */

uint32_t busPIOemptyMask, busPIOstallMask;
static io_ro_32 volatile *rxf;

/* Raw bus capture — pointer aliases for POC pin layout.
 * With in_pin_base=8, PIO wraps the 32 GPIOs so the output is:
 *   bits  0‑15 : GPIO  8‑23  = A0‑A15  (address)
 *   bits 16‑23 : GPIO 24‑31  = WR, M2, junk (control)
 *   bits 24‑31 : GPIO  0‑7   = D0‑D7   (data)
 *
 * On ARM little‑endian:
 *   byte 0‑1 (uint16_t) = address
 *   byte 2              = control
 *   byte 3              = data (opcode)
 */
uint32_t volatile rawBusData;
uint16_t volatile *address = (uint16_t *)(&rawBusData);        /* bytes 0‑1 */
uint8_t volatile  *opcode  = (uint8_t *)(&rawBusData) + 3;     /* byte 3    */
uint8_t volatile  *extra   = (uint8_t *)(&rawBusData) + 2;     /* byte 2    */

/* ================================================================
 *  History ring‑buffer for interrupt detection (read‑ahead of 5)
 * ================================================================ */

uint32_t history[256];
uint volatile cycleIndex;
uint8_t volatile *historyIndex = (uint8_t *)&cycleIndex;
uint volatile gb_div;
static uint8_t readAheadIndex;

/* ================================================================
 *  Mutex / state
 * ================================================================ */

mutex_t cpubusMutex;

bool volatile running  = false;
const volatile char *error;
int volatile errorOpcode;
static uint delayedOpcodeCount = 0;

/* Debug state — written here, printed by Core 0 */
volatile int      emu_debug_state = EMU_STATE_IDLE;
volatile uint32_t emu_debug_cycle_ratio = 0;
volatile uint32_t emu_debug_first_raw = 0;
volatile uint32_t emu_debug_vblank_count = 0;
volatile const char *emu_debug_error = NULL;
volatile uint32_t emu_debug_opcode_count = 0;
volatile uint16_t emu_debug_last_addr = 0;
volatile uint8_t  emu_debug_last_opcode = 0;
volatile uint16_t emu_debug_sp = 0;
volatile uint32_t emu_debug_stall_count = 0;
volatile uint32_t emu_debug_fifo_level = 0;

volatile uint32_t emu_trace_raw[EMU_TRACE_SIZE];
volatile uint32_t emu_trace_count = 0;
volatile uint32_t emu_trace_ops[EMU_OP_TRACE_SIZE];
volatile uint32_t emu_early_bus[EMU_EARLY_BUS_SIZE];
volatile uint32_t emu_early_bus_count = 0;

/* ================================================================
 *  Memory (full 64 KB)
 * ================================================================ */

uint8_t volatile memory[0x010000];

/* OAM DMA via RP2040 DMA channel — fast mem‑to‑mem copy */
static int oamDmaChannel;
static dma_channel_config oamDmaConfig;

bool cartridgeDMA  = false;
uint cartridgeDMAsrc;
uint cartridgeDMAdst;

uint ignoreCycles;

/* ================================================================
 *  CPU registers
 * ================================================================ */

uint8_t registers[8];
uint8_t *c  = registers;
uint8_t *b  = registers + 1;
uint8_t *e  = registers + 2;
uint8_t *d  = registers + 3;
uint8_t *l  = registers + 4;
uint8_t *h  = registers + 5;
uint8_t *a  = registers + 6;
uint16_t *bc = (uint16_t *)registers;
uint16_t *de = (uint16_t *)(registers + 2);
uint16_t *hl = (uint16_t *)(registers + 4);
uint16_t sp  = 0;

uint32_t flags;
uint8_t *Z = (uint8_t *)(&flags);
uint8_t *N = (uint8_t *)(&flags) + 1;
uint8_t *H = (uint8_t *)(&flags) + 2;
uint8_t *C = (uint8_t *)(&flags) + 3;

bool interruptsEnabled;
uint interruptsEnableCycle;

/* ================================================================
 *  Minimal PPU timing state
 * ================================================================ */

int y;
uint lineCycle;
int volatile vblankOffset;

volatile bool bgAndWindowDisplay;
volatile bool objEnable;
volatile uint objSize;
volatile bool bgTileMap9C00;
volatile bool tileData8000;
volatile bool windowEnable;
volatile bool windowTileMap9C00;
volatile bool lcdAndPpuEnable;

volatile uint8_t paletteBG[4];
volatile uint8_t paletteOBP0[4];
volatile uint8_t paletteOBP1[4];

/* ================================================================
 *  Frame signal
 * ================================================================ */

volatile bool emu_new_frame = false;

/* ================================================================
 *  Game‑specific info (defaults — no game detection)
 * ================================================================ */

GameInfo gameInfo;

/* ================================================================
 *  PIO setup — POC pin layout
 *
 *  GPIO 0‑7  : Data   D0‑D7
 *  GPIO 8‑23 : Address A0‑A15
 *  GPIO 24   : WR
 *  GPIO 25   : M2 (CLK)
 *
 *  With in_pin_base=8, PIO wraps the 32 GPIOs producing:
 *   bits  0‑15 : A0‑A15  (address)  — bytes 0‑1
 *   bits 16‑23 : WR, M2, junk       — byte  2 (ctrl/extra)
 *   bits 24‑31 : D0‑D7             — byte  3 (opcode/data)
 * ================================================================ */

static void setupPIO(void)
{
    for (int i = 0; i < 28; i++)
        gpio_init(i);
    uint offset = pio_add_program(BUS_PIO, &memoryBus_program);
    memoryBus_program_init(BUS_PIO, BUS_SM, offset, (float)clock_get_hz(clk_sys) / 10e6);
    pio_sm_set_enabled(BUS_PIO, BUS_SM, true);
    busPIOemptyMask = 1u << (PIO_FSTAT_RXEMPTY_LSB + BUS_SM);
    busPIOstallMask = 1u << (PIO_FDEBUG_RXSTALL_LSB + BUS_SM);
    rxf = BUS_PIO->rxf + BUS_SM;
}

/* ================================================================
 *  OAM DMA helpers
 * ================================================================ */

static void setupOamDMA(void)
{
    oamDmaChannel = dma_claim_unused_channel(true);
    oamDmaConfig  = dma_channel_get_default_config(oamDmaChannel);
    channel_config_set_read_increment(&oamDmaConfig, true);
    channel_config_set_write_increment(&oamDmaConfig, true);
}

void dmaToOAM(uint16_t source)
{
    if (dma_channel_is_busy(oamDmaChannel)) {
        stop("DMA started while channel busy.");
        return;
    }
    dma_channel_configure(oamDmaChannel, &oamDmaConfig,
                          &memory[0xfe00], &memory[source],
                          0xa0 / 4, true);
}

void stop(const char *errorMsg)
{
    if (running) {
        running   = false;
        error     = errorMsg;
    }
}

/* ================================================================
 *  Reset to post‑boot‑ROM state
 * ================================================================ */

static void reset(void)
{
    cycleIndex     = 0;
    readAheadIndex = HISTORY_READAHEAD;
    gb_div         = cycleIndex - 0x0000ab00u; /* DIV starts at 0xAB */

    ignoreCycles   = 0;
    error          = NULL;
    errorOpcode    = -1;

    /* Registers after boot ROM */
    *a = 0x01;
    *b = 0x00;
    *c = 0x13;
    *d = 0x00;
    *e = 0xd8;
    *h = 0x01;
    *l = 0x4d;
    sp = 0xfffe;
    flags = 0x01010001;

    interruptsEnabled    = false;
    interruptsEnableCycle = 0;

    cartridgeDMA = false;

    emu_new_frame = false;

    /* Minimal PPU state */
    y            = 0;
    lineCycle    = 0;
    vblankOffset = 0;

    memset((void *)memory, 0, sizeof(memory));

    toMemory(0xff04, 0xab); /* DIV  */
    toMemory(0xff05, 0x00); /* TIMA */
    toMemory(0xff06, 0x00); /* TMA  */
    toMemory(0xff07, 0x00); /* TAC  */
    toMemory(0xff10, 0x80); /* NR10 */
    toMemory(0xff11, 0xbf); /* NR11 */
    toMemory(0xff12, 0xf3); /* NR12 */
    toMemory(0xff14, 0xbf); /* NR14 */
    toMemory(0xff16, 0x3f); /* NR21 */
    toMemory(0xff17, 0x00); /* NR22 */
    toMemory(0xff19, 0xbf); /* NR24 */
    toMemory(0xff1a, 0x7f); /* NR30 */
    toMemory(0xff1b, 0xff); /* NR31 */
    toMemory(0xff1c, 0x9f); /* NR32 */
    toMemory(0xff1e, 0xbf); /* NR34 */
    toMemory(0xff20, 0xff); /* NR41 */
    toMemory(0xff21, 0x00); /* NR42 */
    toMemory(0xff22, 0x00); /* NR43 */
    toMemory(0xff23, 0xbf); /* NR44 */
    toMemory(0xff24, 0x77); /* NR50 */
    toMemory(0xff25, 0xf3); /* NR51 */
    toMemory(0xff26, 0xf1); /* NR52 */
    toMemory(0xff40, 0x91); /* LCDC */
    toMemory(0xff42, 0x00); /* SCY  */
    toMemory(0xff43, 0x00); /* SCX  */
    toMemory(0xff45, 0x00); /* LYC  */
    toMemory(0xff47, 0xfc); /* BGP  */
    toMemory(0xff48, 0xff); /* OBP0 */
    toMemory(0xff49, 0xff); /* OBP1 */
    toMemory(0xff4a, 0x00); /* WY   */
    toMemory(0xff4b, 0x00); /* WX   */
    toMemory(0xffff, 0x00); /* IE   */

    /* Default game info — no special fixes */
    gameInfo.disableStatSyncs = false;
    gameInfo.disableLySyncs   = false;
    gameInfo.useImmediateIRQ  = false;
    gameInfo.dmaFix           = 0x0000;
    gameInfo.branchBasedFixes[0].jumpAddress = 0x0000;
    gameInfo.writeRegistersDuringDMA[0] = 0x00;
}

/* ================================================================
 *  Minimal PPU step — just advance the Y counter
 * ================================================================ */

void updateMinimalPPU(void)
{
    int adjusted = (int)cycleIndex + vblankOffset;
    if (adjusted < 0) adjusted += CYCLES_PER_FRAME;
    uint frameCycle = (uint)adjusted % CYCLES_PER_FRAME;
    y         = frameCycle / CYCLES_PER_LINE;
    lineCycle = frameCycle % CYCLES_PER_LINE;
}

/* ================================================================
 *  Substitute bus data with local RAM copy for internal addresses
 *
 *  The DMG data bus does not show correct data for reads from
 *  VRAM, WRAM, Echo‑RAM, OAM, IO or HRAM — only cartridge ROM
 *  (0x0000‑0x7FFF) and external RAM (0xA000‑0xBFFF) are reliable.
 * ================================================================ */

static void substitudeBusdataFromMemory(void)
{
    if ((*address & 0x8000) != 0 && ((*address & 0xe000) != 0xa000)) {
        /* Data is at byte 3 of rawBusData (bits 24‑31) */
        *opcode = memory[*address];
        history[*historyIndex] = rawBusData;
    }
}

/* ================================================================
 *  getNextFromBus — fetch one bus cycle from PIO
 *
 *  Includes halt detection via SysTick (same as GBInterceptor).
 * ================================================================ */

void getNextFromBus(void)
{
    while ((BUS_PIO->fstat & busPIOemptyMask) != 0) {
        /* PIO FIFO empty ⇒ wait */
        if (systick_hw->csr & 0x00010000) {
            if (running) {
                delayedOpcodeCount++;
                if (delayedOpcodeCount > 3) {
                    /* Clock is gone — generate synthetic cycles (HALT/STOP) */
                    if ((uint8_t)(history[readAheadIndex] >> 24) != 0x76
                     && (uint8_t)(history[(uint8_t)(readAheadIndex - 1)] >> 24) == 0x76) {
                        history[readAheadIndex] = history[(uint8_t)(readAheadIndex - 1)];
                    }
                    cycleIndex++;
                    readAheadIndex++;
                    history[readAheadIndex] = history[(uint8_t)(readAheadIndex - 1)];
                    rawBusData = history[*historyIndex];
                    substitudeBusdataFromMemory();
                    if (delayedOpcodeCount > 2 * CYCLES_PER_FRAME) {
                        stop("Halt timed out.");
                    }
                    return;
                }
            } else {
                return; /* Not yet running — just return */
            }
        }
    }

    delayedOpcodeCount = 0;
    cycleIndex++;
    readAheadIndex++;

    history[readAheadIndex] = *rxf;

    /* Capture first N raw PIO reads after main loop starts */
    if (emu_early_bus_count < EMU_EARLY_BUS_SIZE)
        emu_early_bus[emu_early_bus_count++] = history[readAheadIndex];

    rawBusData = history[*historyIndex];
    substitudeBusdataFromMemory();
}

/* ================================================================
 *  Main emulator loop — runs on Core 1
 * ================================================================ */

void emu_core1_entry(void)
{
    emu_debug_state = EMU_STATE_STARTED;
    setupPIO();
    setupOamDMA();
    mutex_init(&cpubusMutex);
    mutex_enter_blocking(&cpubusMutex);

    while (1) {
        reset();

        /* Drain FIFO cleanly: stop PIO SM, empty FIFO, restart SM.
         * reset() takes ~70 µs; during that time ~70 GB bus cycles
         * overflow the 16-deep FIFO.  If we drain while the SM runs,
         * the stalled `in` instruction immediately re-pushes a sample
         * captured at an arbitrary moment (not on M2 edge), injecting
         * garbage.  Stopping the SM first avoids this.               */
        pio_sm_set_enabled(BUS_PIO, BUS_SM, false);        /* stop SM */
        while (!(BUS_PIO->fstat & busPIOemptyMask))        /* drain   */
            (void)*rxf;
        BUS_PIO->fdebug = busPIOstallMask;                 /* clear RXSTALL */
        pio_sm_restart(BUS_PIO, BUS_SM);                   /* reset SM PC   */
        pio_sm_set_enabled(BUS_PIO, BUS_SM, true);         /* re-enable     */

        /* ---- Pre-calibration setup (slow ops done BEFORE timing-critical path) ---- */
        emu_debug_state = EMU_STATE_WAITING_0100;
        emu_debug_error = NULL;
        emu_debug_opcode_count = 0;
        emu_debug_stall_count = 0;
        emu_early_bus_count = 0;
        memset((void*)emu_trace_ops, 0, sizeof(emu_trace_ops));

        /* ---- Wait for 0x0100 (cartridge entry) & calibrate cycleRatio ---- */
        uint leadIn = CYCLE_RATIO_STATISTIC_SKIP;
        uint count  = CYCLE_RATIO_STATISTIC_SIZE;
        systick_hw->rvr = 0x00FFFFFF;
        systick_hw->csr = 0x4;
        bool first_raw_captured = false;
        do {
            getNextFromBus();
            if (!first_raw_captured) {
                emu_debug_first_raw = history[readAheadIndex];
                first_raw_captured = true;
            }
            if (leadIn) {
                leadIn--;
                if (!leadIn)
                    systick_hw->csr = 0x5;
            } else if (count) {
                count--;
                if (!count) {
                    cycleRatio = (0x00FFFFFF - systick_hw->cvr) / CYCLE_RATIO_STATISTIC_SIZE;
                    systick_hw->rvr = cycleRatio - 1;
                }
            }
        } while (*address != 0x0100);

        /* ---- Transition to main loop ----
         * CRITICAL: Match GBInterceptor — ZERO slow code between
         * calibration exit and main loop.  The 4‑deep PIO FIFO fills
         * in ~4 µs.  Any getNextFromBus() drain calls here advance
         * the emulator past 0x0100, desynchronizing execution.
         * Only fast register stores allowed here.                     */
        emu_debug_cycle_ratio = cycleRatio;
        emu_debug_fifo_level = (BUS_PIO->flevel >> (BUS_SM * 8 + 4)) & 0xF;
        emu_debug_state = EMU_STATE_RUNNING;
        emu_early_bus_count = 0;  /* reset so main loop captures fresh */
        running = true;
        BUS_PIO->fdebug = busPIOstallMask; /* Clear stall flag */

        /* ---- Main opcode‑following loop ---- */
        while (running) {

            /* --- Ignore events during OAM DMA --- */
            while (ignoreCycles) {
                getNextFromBus();
                if (cartridgeDMA && *address == cartridgeDMAsrc) {
                    memory[cartridgeDMAdst] = *opcode;
                    cartridgeDMAsrc++;
                    cartridgeDMAdst++;
                    if (cartridgeDMAdst >= 0xfea0)
                        cartridgeDMA = false;
                }
                ignoreCycles--;
                if (ignoreCycles == 10) {
                    for (uint i = 0; i < DMA_REGISTER_MAP_SIZE; i += 2) {
                        if (gameInfo.writeRegistersDuringDMA[i] == 0x00)
                            break;
                        toMemory(0xff00 | gameInfo.writeRegistersDuringDMA[i + 1],
                                 memory[0xff00 | gameInfo.writeRegistersDuringDMA[i]]);
                    }
                } else if (ignoreCycles == 0) {
                    bool synchronized = false;
                    int wait = 0;
                    while (!synchronized) {
                        if (*address == sp) {
                            synchronized = true;
                            getNextFromBus();
                            getNextFromBus();
                            getNextFromBus();
                            sp += 2;
                            break;
                        } else if (gameInfo.dmaFix != 0x0000 && *address == gameInfo.dmaFix) {
                            synchronized = true;
                            break;
                        }
                        getNextFromBus();
                        wait++;
                        if (wait > 100) {
                            stop("Could not find a ret after DMA.");
                            break;
                        }
                    }
                }
            }

            /* --- Interrupt detection via read‑ahead pattern matching --- */
            if ((history[readAheadIndex] & 0x0000ffc7) == 0x0040) {
                updateMinimalPPU(); /* Need y & lineCycle for vblank sync */
                uint16_t pushAddr1 = (uint16_t)history[(uint8_t)(readAheadIndex - 2)];
                uint16_t pushAddr2 = (uint16_t)history[(uint8_t)(readAheadIndex - 1)];
                bool spMatch = (pushAddr1 == sp - 1 && pushAddr2 == sp - 2);
                /* Fallback: if SP doesn't match but the two push addresses are
                   consecutive and look like a stack push, resync SP from the bus */
                bool spResync = false;
                if (!spMatch && pushAddr1 == pushAddr2 + 1) {
                    sp = pushAddr2;  /* resync SP to what the bus shows */
                    spResync = true;
                }
                if (spMatch || spResync)
                {
                    uint16_t oldAddress = (uint16_t)history[(uint8_t)(*historyIndex - 1)];
                    toMemory(--sp, oldAddress >> 8);
                    toMemory(--sp, (uint8_t)oldAddress);
                    getNextFromBus();
                    getNextFromBus();
                    getNextFromBus();
                    getNextFromBus();
                    getNextFromBus();

                    if (interruptsEnabled &&
                        (cycleIndex - interruptsEnableCycle > 16 || gameInfo.useImmediateIRQ))
                    {
                        if (*address == 0x0040) {
                            /* VBlank interrupt — sync our Y counter */
                            vblankOffset = (144 - y) * CYCLES_PER_LINE - lineCycle - 6;
                            if (vblankOffset > CYCLES_PER_FRAME / 2)
                                vblankOffset -= CYCLES_PER_FRAME;
                            /* Signal Core 0 that a frame boundary occurred */
                            emu_new_frame = true;
                            emu_debug_vblank_count++;
                        } else if (*address == 0x0048
                                && ((memory[0xff41] & 0b01111000) == 0b01000000))
                        {
                            vblankOffset = (memory[0xff45] - y) * CYCLES_PER_LINE - lineCycle - 6;
                            if (vblankOffset > CYCLES_PER_FRAME / 2)
                                vblankOffset -= CYCLES_PER_FRAME;
                            else if (vblankOffset < -CYCLES_PER_FRAME / 2)
                                vblankOffset += CYCLES_PER_FRAME;
                        }
                    }
                    interruptsEnabled = false;
                }
            }

            /* --- Execute one opcode --- */
            if (emu_debug_opcode_count < EMU_OP_TRACE_SIZE)
                emu_trace_ops[emu_debug_opcode_count] = ((uint32_t)*address << 16) | *opcode;

            /* Clear stall flag BEFORE opcode, check AFTER.
             * This catches only stalls during THIS opcode's
             * getNextFromBus() calls, ignoring stale flags
             * from calibration or earlier transitions.       */
            BUS_PIO->fdebug = busPIOstallMask;
            (*opcodes[*opcode])();
            emu_debug_opcode_count++;

            if (BUS_PIO->fdebug & busPIOstallMask) {
                emu_debug_stall_count++;
                /* Don't stop immediately — count stalls and check
                 * if bus data shows actual lost cycles.  A single
                 * RXSTALL can be a harmless transient if the PIO SM
                 * catches up before the next getNextFromBus().  Only
                 * stop after several consecutive stalls.           */
                if (emu_debug_stall_count >= 10) {
                    errorOpcode = *opcode;
                    stop("PIO stalled (10x).");
                }
            } else {
                emu_debug_stall_count = 0;
            }

            /* PPU Y counter is updated lazily — only when
             * y / lineCycle are actually needed (interrupt
             * handler, LY/STAT reads).  Keeping divisions
             * out of the hot path avoids FIFO overflow.    */
        }

        /* Collect a few more cycles for context on error */
        if (error != NULL) {
            for (uint8_t i = 0; i < 5; i++)
                getNextFromBus();

            /* Populate debug state from current context — not on the hot path */
            emu_debug_last_addr = *address;
            emu_debug_last_opcode = *opcode;
            emu_debug_sp = sp;

            /* Dump last EMU_TRACE_SIZE entries from history ring buffer */
            uint8_t idx = readAheadIndex;
            uint32_t n = EMU_TRACE_SIZE;
            for (uint32_t i = n; i > 0; i--) {
                uint8_t hi = (uint8_t)(idx - i + 1);
                emu_trace_raw[n - i] = history[hi];
            }
            emu_trace_count = n;

            emu_debug_error = error;
            emu_debug_state = EMU_STATE_ERROR;

            /* Let Core 0 read the stable diagnostic data before we
             * restart the while(1) loop and overwrite everything.
             * GBInterceptor uses mutex_exit + sleep_ms(20) + mutex_enter
             * for the same purpose.                                     */
            sleep_ms(50);
        }
    }
}
