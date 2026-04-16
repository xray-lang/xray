/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_jit_debug.h - JIT debugging infrastructure
 *
 * KEY CONCEPT:
 *   Registry of JIT-compiled code regions for crash diagnostics.
 *   SIGSEGV handler that maps crash PC to JIT function + offset.
 *   Disassembly dump of compiled functions.
 *
 * RELATED MODULES:
 *   - xir_arm64_disasm.h: ARM64 instruction decoder
 *   - xir_codegen.h: code generation results
 */

#ifndef XIR_JIT_DEBUG_H
#define XIR_JIT_DEBUG_H

#include <stdint.h>
#include <stdbool.h>
#include "../base/xdefs.h"

#define JIT_DEBUG_MAX_REGIONS 256

typedef struct {
    const char *name;      // function name (borrowed, must outlive)
    void       *code;      // start of executable code
    uint32_t    code_size; // size in bytes
    uint32_t    fast_entry_offset; // instruction offset to fast entry
} JitCodeRegion;

// Register a JIT-compiled code region for crash diagnostics
XR_FUNC void jit_debug_register(const char *name, void *code, uint32_t size,
                        uint32_t fast_entry_offset);

// Install SIGSEGV/SIGBUS handler that reports JIT crash location
XR_FUNC void jit_debug_install_crash_handler(void);

// Dump disassembly of a JIT-compiled function to stderr
XR_FUNC void jit_debug_dump(const char *name, const void *code, uint32_t size,
                    uint32_t fast_entry_offset);

// Lookup which JIT function contains a given PC (NULL if not found)
XR_FUNC const JitCodeRegion *jit_debug_lookup(const void *pc);

/* ========== Guard Page Safepoint ========== */

// Allocate a guard page (PROT_READ). Returns mmap'd address, NULL on failure.
XR_FUNC void *jit_guard_page_alloc(void);

// Free a guard page allocated by jit_guard_page_alloc.
XR_FUNC void jit_guard_page_free(void *page);

// Arm guard page: mprotect PROT_NONE, next LDR faults.
XR_FUNC void jit_guard_page_arm(void *page);

// Disarm guard page: mprotect PROT_READ, LDR succeeds.
XR_FUNC void jit_guard_page_disarm(void *page);

// Initialize the global safepoint trampoline (call once at JIT init).
XR_FUNC void jit_guard_page_init_trampoline(void);

#endif // XIR_JIT_DEBUG_H
