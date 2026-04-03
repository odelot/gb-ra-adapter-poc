/*
 * GB Opcodes header — adapted from GBInterceptor opcodes.h
 */

#ifndef GB_OPCODES_H
#define GB_OPCODES_H

#include "pico/stdlib.h"

extern void (*opcodes[])();
void toMemory(uint16_t address, uint8_t data);

extern bool blockVRAMWrites;

#endif /* GB_OPCODES_H */
