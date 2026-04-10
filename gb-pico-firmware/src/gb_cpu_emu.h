/*
 * GB CPU Emulator - Adapted from GBInterceptor (cpubus.h + ppu.h subset + game_detection.h subset)
 *
 * Tracks Game Boy CPU state by observing bus activity via PIO.
 * Provides full 64KB memory mirror including HRAM (0xFF80-0xFFFE)
 * for RetroAchievements memory inspection.
 *
 * Based on GBInterceptor by Sebastian Staacks
 * https://github.com/Staacks/GBInterceptor
 */

#ifndef GB_CPU_EMU_H
#define GB_CPU_EMU_H

#include "pico/stdlib.h"
#include "pico/mutex.h"
#include "hardware/pio.h"

/* ---------- PPU timing constants (needed for opcode sync logic) ---------- */

#define SCREEN_H 144
#define CYCLES_PER_FRAME 17556
#define CYCLES_PER_LINE 114

/* ---------- PIO / Bus ---------- */

#define BUS_PIO pio0
#define BUS_SM 0

extern uint32_t cycleRatio;

extern volatile bool running;
extern volatile const char *error;
extern volatile int errorOpcode;

extern uint32_t busPIOemptyMask, busPIOstallMask;

extern uint32_t volatile rawBusData;
extern uint8_t volatile *opcode;
extern uint16_t volatile *address;
extern uint8_t volatile *extra;

/* ---------- History / Readahead ---------- */

#define HISTORY_READAHEAD 5
extern uint32_t history[];
extern uint volatile cycleIndex;
extern uint8_t volatile *historyIndex;
extern uint volatile gb_div;

/* ---------- Memory (full 64 KB mirror) ---------- */

extern volatile uint8_t memory[];

/* ---------- CPU Registers ---------- */

extern uint8_t registers[];
extern uint8_t *b, *c, *d, *e, *h, *l, *a;
extern uint16_t *bc, *de, *hl;
extern uint16_t sp;

/* ---------- CPU Flags ---------- */

extern uint32_t flags;
extern uint8_t *Z, *N, *H, *C;

/* ---------- Interrupts ---------- */

extern bool interruptsEnabled;
extern uint interruptsEnableCycle;

/* ---------- DMA state ---------- */

extern uint ignoreCycles;
extern bool cartridgeDMA;
extern uint cartridgeDMAsrc;
extern uint cartridgeDMAdst;

/* ---------- Minimal PPU timing state ---------- */

extern int y;
extern uint lineCycle;
extern int volatile vblankOffset;

/* PPU control flags (set via IO register writes in toMemory) */
extern volatile bool bgAndWindowDisplay;
extern volatile bool objEnable;
extern volatile uint objSize;
extern volatile bool bgTileMap9C00;
extern volatile bool tileData8000;
extern volatile bool windowEnable;
extern volatile bool windowTileMap9C00;
extern volatile bool lcdAndPpuEnable;

/* Palette data (stored by toMemory, not rendered) */
extern volatile uint8_t paletteBG[];
extern volatile uint8_t paletteOBP0[];
extern volatile uint8_t paletteOBP1[];

/* ---------- Frame signal for Core 0 ---------- */

extern volatile bool emu_new_frame;

/* ---------- Debug state (written by Core 1, read/printed by Core 0) ---- */

#define EMU_STATE_IDLE          0
#define EMU_STATE_STARTED       1
#define EMU_STATE_WAITING_0100  2
#define EMU_STATE_CALIBRATED    3
#define EMU_STATE_RUNNING       4
#define EMU_STATE_ERROR         5

extern volatile int      emu_debug_state;
extern volatile uint32_t emu_debug_cycle_ratio;
extern volatile uint32_t emu_debug_first_raw;
extern volatile uint32_t emu_debug_vblank_count;
extern volatile const char *emu_debug_error;
extern volatile uint32_t emu_debug_opcode_count;
extern volatile uint16_t emu_debug_last_addr;
extern volatile uint8_t  emu_debug_last_opcode;
extern volatile uint16_t emu_debug_sp;
extern volatile uint32_t emu_debug_stall_count;
extern volatile uint32_t emu_debug_fifo_level;   /* FIFO level when opcode loop starts */
extern volatile uint32_t emu_debug_sp_resync_count; /* # of times SP was resynced from bus */

/* Bus history dump on error (last N entries from ring buffer) */
#define EMU_TRACE_SIZE 32
extern volatile uint32_t emu_trace_raw[EMU_TRACE_SIZE]; /* history dump on error */
extern volatile uint32_t emu_trace_count;                /* entries captured */

/* First N opcode executions (addr<<16 | opcode) — captured at start only */
#define EMU_OP_TRACE_SIZE 32
extern volatile uint32_t emu_trace_ops[EMU_OP_TRACE_SIZE];

/* First N bus reads after 0x0100 — captures readahead entries in order */
#define EMU_EARLY_BUS_SIZE 64
extern volatile uint32_t emu_early_bus[EMU_EARLY_BUS_SIZE];
extern volatile uint32_t emu_early_bus_count;

/* ---------- Game-specific configuration ---------- */

#define BRANCH_BASED_FIX_LIST_SIZE 3
#define DMA_REGISTER_MAP_SIZE 20

typedef enum { nop, set, and, or, xor, sync } FixMethod;

typedef struct {
    uint16_t jumpAddress;
    uint16_t fixTarget;
    FixMethod takenMethod;
    uint8_t takenValue;
    FixMethod notTakenMethod;
    uint8_t notTakenValue;
} BranchBasedFix;

typedef struct {
    bool disableStatSyncs;
    bool disableLySyncs;
    bool useImmediateIRQ;
    uint16_t dmaFix;
    BranchBasedFix branchBasedFixes[BRANCH_BASED_FIX_LIST_SIZE];
    uint8_t writeRegistersDuringDMA[DMA_REGISTER_MAP_SIZE];
} GameInfo;

extern GameInfo gameInfo;

/* ---------- Mutex ---------- */

extern mutex_t cpubusMutex;

/* ---------- Public Functions ---------- */

void getNextFromBus();
void updateMinimalPPU(void);
void dmaToOAM(uint16_t source);
void stop(const char *errorMsg);

/* Entry point for Core 1 — runs the CPU emulator loop */
void emu_core1_entry(void);

#endif /* GB_CPU_EMU_H */
