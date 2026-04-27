/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_jit_debug.c - JIT debugging infrastructure implementation
 *
 * KEY CONCEPT:
 *   Maintains a registry of JIT code regions. On SIGSEGV/SIGBUS,
 *   looks up the faulting PC in the registry and prints the function
 *   name, offset, and surrounding disassembly for quick diagnosis.
 */

#ifdef __aarch64__

#include "xir_jit_debug.h"
#include "../base/xchecks.h"
#include "xir_arm64_disasm.h"
#include "xir_arm64.h"
#include "xir_codegen.h"
#include "xir_offsets.h"
#include "xir_jit_runtime.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../os/os_thread.h"

#if defined(XR_OS_MACOS)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <unistd.h>
#endif  // ========== Code Region Registry ==========

static JitCodeRegion g_regions[JIT_DEBUG_MAX_REGIONS];
static uint32_t g_nregions = 0;

void jit_debug_register(const char *name, void *code, uint32_t size, uint32_t fast_entry_offset) {
    XR_DCHECK(name != NULL, "jit_debug_register: NULL name");
    XR_DCHECK(code != NULL, "jit_debug_register: NULL code");
    if (g_nregions >= JIT_DEBUG_MAX_REGIONS) {
        fprintf(stderr, "[JIT-debug] region registry full, cannot register %s\n",
                name ? name : "?");
        return;
    }
    JitCodeRegion *r = &g_regions[g_nregions++];
    r->name = name;
    r->code = code;
    r->code_size = size;
    r->fast_entry_offset = fast_entry_offset;
}

const JitCodeRegion *jit_debug_lookup(const void *pc) {
    for (uint32_t i = 0; i < g_nregions; i++) {
        const JitCodeRegion *r = &g_regions[i];
        uintptr_t start = (uintptr_t) r->code;
        uintptr_t end = start + r->code_size;
        if ((uintptr_t) pc >= start && (uintptr_t) pc < end)
            return r;
    }
    return NULL;
}

/* ========== Disassembly Dump ========== */

void jit_debug_dump(const char *name, const void *code, uint32_t size, uint32_t fast_entry_offset) {
    XR_DCHECK(code != NULL, "jit_debug_dump: NULL code");
    uint32_t n_inst = size / 4;
    const uint32_t *insts = (const uint32_t *) code;

    fprintf(stderr, "\n===== JIT disasm: %s (%u bytes, %u instructions) =====\n", name ? name : "?",
            size, n_inst);
    fprintf(stderr, "  normal_entry: 0x0000\n");
    fprintf(stderr, "  fast_entry:   0x%04x\n", fast_entry_offset);
    fprintf(stderr, "-----\n");

    a64_disasm_dump(stderr, insts, n_inst, 0);

    fprintf(stderr, "===== end %s =====\n\n", name ? name : "?");
}

/* ========== Guard Page Safepoint ========== */

static void *g_safepoint_trampoline = NULL;  // global trampoline code (mmap'd executable)
static uint32_t g_trampoline_size = 0;

void *jit_guard_page_alloc(void) {
    void *page = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (page == MAP_FAILED)
        return NULL;
    // Start disarmed (PROT_READ). Sysmon will arm periodically.
    return page;
}

void jit_guard_page_free(void *page) {
    if (page)
        munmap(page, 4096);
}

void jit_guard_page_arm(void *page) {
    if (page)
        mprotect(page, 4096, PROT_NONE);
}

void jit_guard_page_disarm(void *page) {
    if (page)
        mprotect(page, 4096, PROT_READ);
}

/*
 * Generate global safepoint trampoline as executable ARM64 code.
 *
 * Entry contract (set by signal handler via ucontext redirect):
 *   x19 = CORO_REG (coroutine pointer)
 *   x28 = JIT_CTX_REG (jit_ctx pointer)
 *   x20 = SAFEPT_PAGE_REG (guard page pointer)
 *   jit_ctx->safepoint_return_pc = address to resume after safepoint
 *   jit_ctx->active_safepoint_id = smap_id (set by signal handler)
 *
 * Flow:
 *   1. Save caller-saved registers (x0-x15, LR, d0-d7)
 *   2. Call xr_coro_gc_safepoint(coro)
 *   3. Restore registers
 *   4. Jump to saved return PC
 */
void jit_guard_page_init_trampoline(void) {
    if (g_safepoint_trampoline)
        return;  // already initialized

    // Allocate buffer for trampoline instructions (stack buffer, copy to mmap later)
    uint32_t code[128];
    A64Buf buf;
    a64_buf_init(&buf, code, 128);

    // Save caller-saved GP (x0-x15, LR) + FP (d0-d7) = 24*8 = 192 bytes
    a64_buf_emit(&buf, a64_sub_imm(A64_SP, A64_SP, 256));
    a64_buf_emit(&buf, a64_stp(A64_X0, A64_X1, A64_SP, 0));
    a64_buf_emit(&buf, a64_stp(A64_X2, A64_X3, A64_SP, 16));
    a64_buf_emit(&buf, a64_stp(A64_X4, A64_X5, A64_SP, 32));
    a64_buf_emit(&buf, a64_stp(A64_X6, A64_X7, A64_SP, 48));
    a64_buf_emit(&buf, a64_stp(A64_X8, A64_X9, A64_SP, 64));
    a64_buf_emit(&buf, a64_stp(A64_X10, A64_X11, A64_SP, 80));
    a64_buf_emit(&buf, a64_stp(A64_X12, A64_X13, A64_SP, 96));
    a64_buf_emit(&buf, a64_stp(A64_X14, A64_X15, A64_SP, 112));
    a64_buf_emit(&buf, a64_str(A64_LR, A64_SP, 128));
    // FP caller-saved d0-d7
    for (int i = 0; i < 8; i++)
        a64_buf_emit(&buf, a64_str_fp(i, A64_SP, 136 + i * 8));

    // Save SP to jit_ctx for GC stack map access
    a64_buf_emit(&buf, a64_str(A64_SP, A64_X28, (int32_t) XIR_JIT_SAFEPOINT_SAVED_SP_OFFSET));

    // Call xr_coro_gc_safepoint(coro)
    a64_buf_emit(&buf, a64_mov(A64_X0, A64_X19));  // x0 = coro
    a64_load_imm64(&buf, A64_X16, (uint64_t) (uintptr_t) xr_coro_gc_safepoint);
    a64_buf_emit(&buf, a64_blr(A64_X16));
    // x0 = return value (0=continue, non-zero=cancel) — ignored for v1

    // Restore FP d0-d7
    for (int i = 0; i < 8; i++)
        a64_buf_emit(&buf, a64_ldr_fp(i, A64_SP, 136 + i * 8));
    // Restore GP
    a64_buf_emit(&buf, a64_ldp(A64_X0, A64_X1, A64_SP, 0));
    a64_buf_emit(&buf, a64_ldp(A64_X2, A64_X3, A64_SP, 16));
    a64_buf_emit(&buf, a64_ldp(A64_X4, A64_X5, A64_SP, 32));
    a64_buf_emit(&buf, a64_ldp(A64_X6, A64_X7, A64_SP, 48));
    a64_buf_emit(&buf, a64_ldp(A64_X8, A64_X9, A64_SP, 64));
    a64_buf_emit(&buf, a64_ldp(A64_X10, A64_X11, A64_SP, 80));
    a64_buf_emit(&buf, a64_ldp(A64_X12, A64_X13, A64_SP, 96));
    a64_buf_emit(&buf, a64_ldp(A64_X14, A64_X15, A64_SP, 112));
    a64_buf_emit(&buf, a64_ldr(A64_LR, A64_SP, 128));
    a64_buf_emit(&buf, a64_add_imm(A64_SP, A64_SP, 256));

    // Jump to saved return PC: jit_ctx->safepoint_return_pc
    a64_buf_emit(&buf, a64_ldr(A64_X16, A64_X28, (int32_t) XIR_JIT_SAFEPOINT_RETURN_PC_OFFSET));
    a64_buf_emit(&buf, a64_br(A64_X16));

    // Copy to executable memory
    g_trampoline_size = buf.count * 4;
    g_safepoint_trampoline =
        mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (g_safepoint_trampoline == MAP_FAILED) {
        g_safepoint_trampoline = NULL;
        fprintf(stderr, "[JIT] FATAL: failed to allocate safepoint trampoline\n");
        return;
    }
    memcpy(g_safepoint_trampoline, code, g_trampoline_size);
    mprotect(g_safepoint_trampoline, 4096, PROT_READ | PROT_EXEC);

    // Clear instruction cache for the trampoline
    __builtin___clear_cache(g_safepoint_trampoline,
                            (char *) g_safepoint_trampoline + g_trampoline_size);
}

/* ========== Crash Handler ========== */

static struct sigaction g_old_sigsegv;
static struct sigaction g_old_sigbus;

/*
 * Try to handle a guard page safepoint fault.
 *
 * Returns true if handled (execution will resume at trampoline).
 * Returns false if this is a real crash.
 */
static bool try_handle_guard_page_fault(void *fault_pc, void *fault_addr, void *ucontext) {
#if defined(__aarch64__) || defined(__arm64__)
    if (!fault_pc || !fault_addr)
        return false;

    ucontext_t *uc = (ucontext_t *) ucontext;

    // Read x28 (JIT_CTX_REG) from ucontext to get jit_ctx
#if defined(XR_OS_MACOS)
    uint64_t x28_val = uc->uc_mcontext->__ss.__x[28];
#elif defined(XR_OS_LINUX)
    uint64_t x28_val = uc->uc_mcontext.regs[28];
#else
    return false;
#endif

    if (!x28_val)
        return false;

    // Verify fault_addr matches jit_ctx->safepoint_page
    XrJitScratch *jit_ctx = (XrJitScratch *) (uintptr_t) x28_val;
    if (fault_addr != jit_ctx->safepoint_page)
        return false;

    // This IS a guard page fault. Disarm first.
    jit_guard_page_disarm(jit_ctx->safepoint_page);

    // Check if fault PC is in JIT code
    const JitCodeRegion *region = jit_debug_lookup(fault_pc);
    if (!region || !g_safepoint_trampoline) {
        // Fault happened in C code (not JIT). Just disarm and retry.
        // The faulting instruction will succeed on retry (page now PROT_READ).
        return true;
    }

    // JIT code fault: redirect to safepoint trampoline.
    // 1. Look up smap_id from fault PC offset in stack map table
    uint32_t fault_offset = (uint32_t) ((uintptr_t) fault_pc - (uintptr_t) region->code);
    XrStackMapTable *smap = (XrStackMapTable *) jit_ctx->active_stack_map;
    if (smap && smap->magic == XR_STACK_MAP_MAGIC) {
        for (uint32_t i = 0; i < smap->count; i++) {
            if (smap->entries[i].pc_offset == fault_offset) {
                jit_ctx->active_safepoint_id = i;
#if defined(XR_OS_MACOS)
                uint64_t fp_val = uc->uc_mcontext->__ss.__fp;
#elif defined(XR_OS_LINUX)
                uint64_t fp_val = uc->uc_mcontext.regs[29];
#endif
                if (fp_val) {
                    *(uint32_t *) ((uintptr_t) fp_val + 168) = i;  // FRAME_SMAP_ID_OFFSET
                }
                break;
            }
        }
    }

    // 2. Save return PC (instruction after the faulting LDR)
    jit_ctx->safepoint_return_pc = (void *) ((uintptr_t) fault_pc + 4);

    // 3. Redirect execution to global safepoint trampoline
#if defined(XR_OS_MACOS)
    uc->uc_mcontext->__ss.__pc = (uint64_t) (uintptr_t) g_safepoint_trampoline;
#elif defined(XR_OS_LINUX)
    uc->uc_mcontext.pc = (uint64_t) (uintptr_t) g_safepoint_trampoline;
#endif

    return true;
#else
    (void) fault_pc;
    (void) fault_addr;
    (void) ucontext;
    return false;
#endif
}

static void jit_crash_handler(int sig, siginfo_t *info, void *ucontext) {
    const char *signame = (sig == SIGSEGV) ? "SIGSEGV" : "SIGBUS";

    // Get faulting PC from ucontext
    void *fault_pc = NULL;
#if defined(__aarch64__) || defined(__arm64__)
    ucontext_t *uc = (ucontext_t *) ucontext;
#if defined(XR_OS_MACOS)
    fault_pc = (void *) uc->uc_mcontext->__ss.__pc;
#elif defined(XR_OS_LINUX)
    fault_pc = (void *) uc->uc_mcontext.pc;
#endif
#elif defined(__x86_64__)
    ucontext_t *uc = (ucontext_t *) ucontext;
#if defined(XR_OS_MACOS)
    fault_pc = (void *) uc->uc_mcontext->__ss.__rip;
#elif defined(XR_OS_LINUX)
    fault_pc = (void *) uc->uc_mcontext.gregs[16];  // REG_RIP
#endif
#endif

    // Try guard page safepoint first (returns true if handled).
    // macOS sends SIGBUS (not SIGSEGV) for mprotect PROT_NONE access violations.
    if ((sig == SIGSEGV || sig == SIGBUS) &&
        try_handle_guard_page_fault(fault_pc, info->si_addr, ucontext))
        return;

    fprintf(stderr, "\n[JIT-CRASH] %s at pc=%p fault_addr=%p\n", signame, fault_pc, info->si_addr);

    // Lookup in JIT code registry
    if (fault_pc) {
        const JitCodeRegion *r = jit_debug_lookup(fault_pc);
        if (r) {
            uint32_t offset = (uint32_t) ((uintptr_t) fault_pc - (uintptr_t) r->code);
            fprintf(stderr, "[JIT-CRASH] In function '%s' at offset 0x%04x (instruction #%u)\n",
                    r->name ? r->name : "?", offset, offset / 4);
            fprintf(stderr, "[JIT-CRASH] Code range: %p - %p (%u bytes)\n", r->code,
                    (char *) r->code + r->code_size, r->code_size);

            // Dump surrounding context: 5 instructions before and after
            uint32_t crash_inst = offset / 4;
            uint32_t n_inst = r->code_size / 4;
            uint32_t start = (crash_inst > 10) ? crash_inst - 10 : 0;
            uint32_t end = (crash_inst + 10 < n_inst) ? crash_inst + 10 : n_inst;

            fprintf(stderr, "[JIT-CRASH] Disassembly around crash point:\n");
            const uint32_t *code = (const uint32_t *) r->code;
            char line[256];
            for (uint32_t i = start; i < end; i++) {
                uint32_t off = i * 4;
                a64_disasm_one(line, sizeof(line), code[i], off);
                fprintf(stderr, "  %s %04x: %08x  %s\n", (i == crash_inst) ? ">>>" : "   ", off,
                        code[i], line);
            }

            // Also dump full disassembly to /tmp
            char fname[256];
            snprintf(fname, sizeof(fname), "/tmp/jit_crash_%s.txt", r->name ? r->name : "unknown");
            FILE *f = fopen(fname, "w");
            if (f) {
                fprintf(f, "JIT crash in '%s' at offset 0x%04x\n\n", r->name ? r->name : "?",
                        offset);
                a64_disasm_dump(f, code, n_inst, 0);
                fclose(f);
                fprintf(stderr, "[JIT-CRASH] Full disassembly saved to %s\n", fname);
            }
        } else {
            fprintf(stderr, "[JIT-CRASH] PC not in any known JIT region\n");

            // Print all known regions for reference
            fprintf(stderr, "[JIT-CRASH] Known JIT regions (%u):\n", g_nregions);
            for (uint32_t i = 0; i < g_nregions; i++) {
                const JitCodeRegion *ri = &g_regions[i];
                fprintf(stderr, "  [%u] %s: %p - %p (%u bytes)\n", i, ri->name ? ri->name : "?",
                        ri->code, (char *) ri->code + ri->code_size, ri->code_size);
            }
        }
    }

#if defined(__aarch64__) || defined(__arm64__)
    // Print key registers for diagnosis
    ucontext_t *uc2 = (ucontext_t *) ucontext;
#if defined(XR_OS_MACOS)
    fprintf(stderr, "[JIT-CRASH] Registers:\n");
    for (int i = 0; i < 29; i++) {
        fprintf(stderr, "  x%-2d = 0x%016llx", i,
                (unsigned long long) uc2->uc_mcontext->__ss.__x[i]);
        if (i % 4 == 3)
            fprintf(stderr, "\n");
    }
    fprintf(stderr, "\n  fp  = 0x%016llx  lr  = 0x%016llx  sp  = 0x%016llx\n",
            (unsigned long long) uc2->uc_mcontext->__ss.__fp,
            (unsigned long long) uc2->uc_mcontext->__ss.__lr,
            (unsigned long long) uc2->uc_mcontext->__ss.__sp);
#endif
#endif

    fprintf(stderr, "[JIT-CRASH] Aborting.\n");
    fflush(stderr);

    // Re-raise with default handler
    struct sigaction sa;
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, NULL);
    raise(sig);
}

// Detect if a debugger (lldb/gdb) is attached to this process.
// Uses macOS sysctl P_TRACED flag; returns false on other platforms.
static bool jit_debugger_attached(void) {
#if defined(XR_OS_MACOS)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    struct kinfo_proc info;
    memset(&info, 0, sizeof(info));
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, NULL, 0) == 0) {
        return (info.kp_proc.p_flag & P_TRACED) != 0;
    }
#endif
    return false;
}

void jit_debug_install_crash_handler(void) {
    if (getenv("XRAY_NO_JIT_CRASH_HANDLER")) {
        fprintf(stderr, "[JIT-debug] crash handler disabled by XRAY_NO_JIT_CRASH_HANDLER\n");
        return;
    }
    if (jit_debugger_attached()) {
        fprintf(stderr, "[JIT-debug] debugger detected, skipping crash handler install\n");
        return;
    }

// Allocate alternate signal stack so handler works during stack overflow
// Use fixed 64KB instead of SIGSTKSZ which is not a compile-time constant on glibc 2.34+
#define JIT_ALT_STACK_SIZE (64 * 1024)
    static char alt_stack_buf[JIT_ALT_STACK_SIZE];
    stack_t ss;
    ss.ss_sp = alt_stack_buf;
    ss.ss_size = JIT_ALT_STACK_SIZE;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0) {
        perror("[JIT-debug] sigaltstack failed");
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = jit_crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &g_old_sigsegv);
    sigaction(SIGBUS, &sa, &g_old_sigbus);

    fprintf(stderr, "[JIT-debug] crash handler installed (with alt stack)\n");
}

#endif  // __aarch64__
