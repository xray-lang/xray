/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_jit_debug.h - JIT debugging infrastructure
 *
 * KEY CONCEPT:
 *   Registry of JIT-compiled code regions for crash diagnostics.
 *   SIGSEGV handler that maps crash PC to JIT function + offset.
 *   Disassembly dump of compiled functions.
 *
 * RELATED MODULES:
 *   - xm_arm64_disasm.h: ARM64 instruction decoder
 *   - xm_codegen.h: code generation results
 */

#ifndef XM_JIT_DEBUG_H
#define XM_JIT_DEBUG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../base/xdefs.h"

#define JIT_DEBUG_MAX_REGIONS 256

typedef struct {
    const char *name;            // function name (borrowed, must outlive)
    void *code;                  // start of executable code
    uint32_t code_size;          // size in bytes
    uint32_t fast_entry_offset;  // instruction offset to fast entry
} JitCodeRegion;

typedef enum {
    JIT_DEBUG_DISASM_ARM64 = 0,
    JIT_DEBUG_DISASM_RISCV64 = 1,
    JIT_DEBUG_DISASM_X64 = 2,
} JitDebugDisasmArch;

/* ========== Platform-independent API ========== */

/* Register a JIT-compiled code region for crash diagnostics.
 * Thread-safe: uses atomic index for concurrent background compilation. */
XR_FUNC void jit_debug_register(const char *name, void *code, uint32_t size,
                                uint32_t fast_entry_offset);

/* Install SIGSEGV/SIGBUS handler that reports JIT crash location.
 * Works on both ARM64 and x64 (register dump is platform-specific). */
XR_FUNC void jit_debug_install_crash_handler(void);

/* Dump code of a JIT-compiled function to stderr with disassembly. */
XR_FUNC void jit_debug_dump(const char *name, const void *code, uint32_t size,
                            uint32_t fast_entry_offset);

XR_FUNC const char *jit_debug_disasm_mcinsn_name(JitDebugDisasmArch arch, uint32_t insn);

/* Decode one x64 instruction and return its mcinsn name.
 * Returns instruction length via *out_len (0 if unrecognized). */
XR_FUNC const char *jit_debug_x64_disasm_one(const uint8_t *code, size_t avail, int *out_len);

XR_FUNC int jit_debug_format_mcinsn(char *buf, size_t bufsz, JitDebugDisasmArch arch,
                                    uint32_t insn);

XR_FUNC bool jit_debug_is_guard_page_fault_addr(const void *fault_addr, const void *page);

/* Lookup which JIT function contains a given PC (NULL if not found).
 * Safe to call from signal handler (lock-free read). */
XR_FUNC const JitCodeRegion *jit_debug_lookup(const void *pc);

/* Log deopt event with generated disasm context.
 * Emits function name, deopt_id, bc_pc, source_line, and surrounding
 * instruction disassembly via xr_log_debug("jit-deopt", ...).
 * source_line: 0 means unknown (pre-linemap callers pass 0). */
XR_FUNC void jit_debug_log_deopt(const char *func_name, const void *code, uint32_t code_size,
                                 uint32_t deopt_id, uint32_t bc_pc, int source_line);

/* ========== Guard Page Safepoint (ARM64 only) ========== */

#ifdef __aarch64__

XR_FUNC void *jit_guard_page_alloc(void);
XR_FUNC void jit_guard_page_free(void *page);
XR_FUNC void jit_guard_page_arm(void *page);
XR_FUNC void jit_guard_page_disarm(void *page);
XR_FUNC void jit_guard_page_init_trampoline(void);

#else /* !__aarch64__ — guard page safepoint not available */

static inline void *jit_guard_page_alloc(void) {
    return NULL;
}
static inline void jit_guard_page_free(void *p) {
    (void) p;
}
static inline void jit_guard_page_arm(void *p) {
    (void) p;
}
static inline void jit_guard_page_disarm(void *p) {
    (void) p;
}
static inline void jit_guard_page_init_trampoline(void) {
}

#endif /* __aarch64__ */

#endif  // XM_JIT_DEBUG_H
