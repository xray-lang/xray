/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xdebug.h - Bytecode debugging tools (disassembler)
 *
 * KEY CONCEPT:
 *   Provides bytecode disassembly for debugging and analysis.
 */

#ifndef XDEBUG_H
#define XDEBUG_H

#include "../runtime/value/xchunk.h"

/* ========== Disassembly API ========== */

// Disassemble entire function prototype
XR_FUNC void xr_disassemble_proto(XrProto *proto, const char *name);

// Disassemble single instruction, returns next instruction offset
XR_FUNC int xr_disassemble_instruction(XrProto *proto, int offset);

// Opcode name lookup now lives in runtime/value/xopcode_info.h (xr_opcode_name).

/* ========== Debug Output ========== */

// Print constants table
XR_FUNC void xr_print_constants(XrProto *proto);

#endif // XDEBUG_H
