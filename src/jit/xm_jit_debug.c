/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_jit_debug.c - JIT debugging infrastructure implementation
 *
 * KEY CONCEPT:
 *   Maintains a registry of JIT code regions. On SIGSEGV/SIGBUS,
 *   looks up the faulting PC in the registry and prints the function
 *   name, offset, and surrounding disassembly for quick diagnosis.
 */

#include "xm_jit_debug.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "xm_offsets.h"
#include "xm_jit_runtime.h"

#ifdef __aarch64__
#include "xm_arm64_disasm.h"
#include "xm_arm64.h"
#include "xm_codegen.h"
#endif

#ifndef _WIN32
#include <signal.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "../os/os_codemem.h"
#include "../os/os_proc.h"
#include "../os/os_thread.h"

// ========== Code Region Registry ==========

/* Region registry — accessed from background compile thread and signal
 * handler; g_nregions uses atomic_uint for safe concurrent registration. */
static JitCodeRegion g_regions[JIT_DEBUG_MAX_REGIONS];
static atomic_uint g_nregions = 0;

XR_FUNC void jit_debug_register(const char *name, void *code, uint32_t size,
                                uint32_t fast_entry_offset) {
    XR_DCHECK(name != NULL, "jit_debug_register: NULL name");
    XR_DCHECK(code != NULL, "jit_debug_register: NULL code");
    uint32_t idx = atomic_fetch_add_explicit(&g_nregions, 1, memory_order_relaxed);
    if (idx >= JIT_DEBUG_MAX_REGIONS) {
        atomic_fetch_sub_explicit(&g_nregions, 1, memory_order_relaxed);
        fprintf(stderr, "[JIT-debug] region registry full, cannot register %s\n",
                name ? name : "?");
        return;
    }
    JitCodeRegion *r = &g_regions[idx];
    r->name = name;
    r->code = code;
    r->code_size = size;
    r->fast_entry_offset = fast_entry_offset;
    atomic_thread_fence(memory_order_release);
}

XR_FUNC const JitCodeRegion *jit_debug_lookup(const void *pc) {
    uint32_t n = atomic_load_explicit(&g_nregions, memory_order_acquire);
    for (uint32_t i = 0; i < n; i++) {
        const JitCodeRegion *r = &g_regions[i];
        uintptr_t start = (uintptr_t) r->code;
        uintptr_t end = start + r->code_size;
        if ((uintptr_t) pc >= start && (uintptr_t) pc < end)
            return r;
    }
    return NULL;
}

/* ========== Disassembly Dump ========== */

XR_FUNC void jit_debug_dump(const char *name, const void *code, uint32_t size,
                            uint32_t fast_entry_offset) {
    XR_DCHECK(code != NULL, "jit_debug_dump: NULL code");

    fprintf(stderr, "\n===== JIT disasm: %s (%u bytes) =====\n", name ? name : "?", size);
    fprintf(stderr, "  normal_entry: 0x0000\n");
    fprintf(stderr, "  fast_entry:   0x%04x\n", fast_entry_offset);
    fprintf(stderr, "-----\n");

#if defined(__aarch64__)
    uint32_t n_inst = size / 4;
    const uint32_t *insts = (const uint32_t *) code;
    a64_disasm_dump(stderr, insts, n_inst, 0);
#elif defined(__x86_64__)
    /* No x64 disassembler yet — dump raw hex bytes */
    const uint8_t *bytes = (const uint8_t *) code;
    for (uint32_t off = 0; off < size; off += 16) {
        fprintf(stderr, "  %04x:", off);
        for (uint32_t j = 0; j < 16 && off + j < size; j++)
            fprintf(stderr, " %02x", bytes[off + j]);
        fprintf(stderr, "\n");
    }
#endif

    fprintf(stderr, "===== end %s =====\n\n", name ? name : "?");
}

/* ========== Guard Page Safepoint (ARM64 only — uses x28/x19 registers) ========== */

#ifdef __aarch64__

static void *g_safepoint_trampoline = NULL; /* global trampoline code (mmap'd executable) */
static uint32_t g_trampoline_size = 0;

void *jit_guard_page_alloc(void) {
    // Start disarmed (R). Sysmon will flip to NONE periodically to arm.
    return xr_os_mem_alloc(4096, XR_MEM_PROT_R);
}

void jit_guard_page_free(void *page) {
    xr_os_mem_free(page, 4096);
}

void jit_guard_page_arm(void *page) {
    xr_os_mem_protect(page, 4096, XR_MEM_PROT_NONE);
}

void jit_guard_page_disarm(void *page) {
    xr_os_mem_protect(page, 4096, XR_MEM_PROT_R);
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
    a64_buf_emit(&buf, a64_str(A64_SP, A64_X28, (int32_t) XM_JIT_SAFEPOINT_SAVED_SP_OFFSET));

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
    a64_buf_emit(&buf, a64_ldr(A64_X16, A64_X28, (int32_t) XM_JIT_SAFEPOINT_RETURN_PC_OFFSET));
    a64_buf_emit(&buf, a64_br(A64_X16));

    // Copy to executable memory via the OS shim.
    g_trampoline_size = buf.count * 4;
    g_safepoint_trampoline = xr_os_codemem_alloc(4096);
    if (g_safepoint_trampoline == NULL) {
        fprintf(stderr, "[JIT] FATAL: failed to allocate safepoint trampoline\n");
        return;
    }
    memcpy(g_safepoint_trampoline, code, g_trampoline_size);
    xr_os_codemem_make_executable(g_safepoint_trampoline, 4096);
    xr_os_codemem_flush_icache(g_safepoint_trampoline, g_trampoline_size);
}

#endif /* __aarch64__ guard page safepoint */

/* ========== Crash Handler (POSIX only — Windows uses SEH) ========== */

#ifndef _WIN32

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

            /* Dump surrounding code context around crash point */
            fprintf(stderr, "[JIT-CRASH] Code around crash point:\n");
#if defined(__aarch64__)
            {
                uint32_t crash_inst = offset / 4;
                uint32_t n_inst = r->code_size / 4;
                uint32_t dstart = (crash_inst > 10) ? crash_inst - 10 : 0;
                uint32_t dend = (crash_inst + 10 < n_inst) ? crash_inst + 10 : n_inst;
                const uint32_t *code = (const uint32_t *) r->code;
                char line[256];
                for (uint32_t i = dstart; i < dend; i++) {
                    uint32_t off = i * 4;
                    a64_disasm_one(line, sizeof(line), code[i], off);
                    fprintf(stderr, "  %s %04x: %08x  %s\n", (i == crash_inst) ? ">>>" : "   ", off,
                            code[i], line);
                }
            }
#elif defined(__x86_64__)
            {
                /* x64: variable-length instructions, dump hex bytes */
                uint32_t dstart = (offset > 32) ? offset - 32 : 0;
                uint32_t dend = (offset + 32 < r->code_size) ? offset + 32 : r->code_size;
                const uint8_t *bytes = (const uint8_t *) r->code;
                for (uint32_t off = dstart; off < dend; off += 16) {
                    fprintf(stderr,
                            "  %s %04x:", (off <= offset && offset < off + 16) ? ">>>" : "   ",
                            off);
                    for (uint32_t j = 0; j < 16 && off + j < dend; j++)
                        fprintf(stderr, " %02x", bytes[off + j]);
                    fprintf(stderr, "\n");
                }
            }
#endif

            /* Save full code dump to /tmp */
            char fname[256];
            snprintf(fname, sizeof(fname), "/tmp/jit_crash_%s.txt", r->name ? r->name : "unknown");
            FILE *f = fopen(fname, "w");
            if (f) {
                fprintf(f, "JIT crash in '%s' at offset 0x%04x\n\n", r->name ? r->name : "?",
                        offset);
#if defined(__aarch64__)
                uint32_t n_inst = r->code_size / 4;
                a64_disasm_dump(f, (const uint32_t *) r->code, n_inst, 0);
#elif defined(__x86_64__)
                const uint8_t *bytes = (const uint8_t *) r->code;
                for (uint32_t off = 0; off < r->code_size; off += 16) {
                    fprintf(f, "  %04x:", off);
                    for (uint32_t j = 0; j < 16 && off + j < r->code_size; j++)
                        fprintf(f, " %02x", bytes[off + j]);
                    fprintf(f, "\n");
                }
#endif
                fclose(f);
                fprintf(stderr, "[JIT-CRASH] Full dump saved to %s\n", fname);
            }
        } else {
            fprintf(stderr, "[JIT-CRASH] PC not in any known JIT region\n");

            // Print all known regions for reference
            uint32_t nr = atomic_load_explicit(&g_nregions, memory_order_acquire);
            fprintf(stderr, "[JIT-CRASH] Known JIT regions (%u):\n", nr);
            for (uint32_t i = 0; i < nr; i++) {
                const JitCodeRegion *ri = &g_regions[i];
                fprintf(stderr, "  [%u] %s: %p - %p (%u bytes)\n", i, ri->name ? ri->name : "?",
                        ri->code, (char *) ri->code + ri->code_size, ri->code_size);
            }
        }
    }

    /* Print key registers for diagnosis */
    {
        ucontext_t *uc2 = (ucontext_t *) ucontext;
        (void) uc2;
#if defined(__aarch64__) && defined(XR_OS_MACOS)
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
#elif defined(__x86_64__) && defined(XR_OS_MACOS)
        fprintf(stderr, "[JIT-CRASH] Registers:\n");
        fprintf(stderr, "  rax = 0x%016llx  rbx = 0x%016llx  rcx = 0x%016llx\n",
                uc2->uc_mcontext->__ss.__rax, uc2->uc_mcontext->__ss.__rbx,
                uc2->uc_mcontext->__ss.__rcx);
        fprintf(stderr, "  rdx = 0x%016llx  rsi = 0x%016llx  rdi = 0x%016llx\n",
                uc2->uc_mcontext->__ss.__rdx, uc2->uc_mcontext->__ss.__rsi,
                uc2->uc_mcontext->__ss.__rdi);
        fprintf(stderr, "  rbp = 0x%016llx  rsp = 0x%016llx  rip = 0x%016llx\n",
                uc2->uc_mcontext->__ss.__rbp, uc2->uc_mcontext->__ss.__rsp,
                uc2->uc_mcontext->__ss.__rip);
        fprintf(stderr, "  r8  = 0x%016llx  r9  = 0x%016llx  r10 = 0x%016llx\n",
                uc2->uc_mcontext->__ss.__r8, uc2->uc_mcontext->__ss.__r9,
                uc2->uc_mcontext->__ss.__r10);
        fprintf(stderr, "  r11 = 0x%016llx  r12 = 0x%016llx  r13 = 0x%016llx\n",
                uc2->uc_mcontext->__ss.__r11, uc2->uc_mcontext->__ss.__r12,
                uc2->uc_mcontext->__ss.__r13);
        fprintf(stderr, "  r14 = 0x%016llx  r15 = 0x%016llx\n", uc2->uc_mcontext->__ss.__r14,
                uc2->uc_mcontext->__ss.__r15);
#elif defined(__x86_64__) && defined(XR_OS_LINUX)
        fprintf(stderr, "[JIT-CRASH] Registers:\n");
        fprintf(stderr, "  rax = 0x%016lx  rbx = 0x%016lx  rcx = 0x%016lx\n",
                (unsigned long) uc2->uc_mcontext.gregs[13],
                (unsigned long) uc2->uc_mcontext.gregs[11],
                (unsigned long) uc2->uc_mcontext.gregs[14]);
        fprintf(stderr, "  rip = 0x%016lx  rsp = 0x%016lx  rbp = 0x%016lx\n",
                (unsigned long) uc2->uc_mcontext.gregs[16],
                (unsigned long) uc2->uc_mcontext.gregs[15],
                (unsigned long) uc2->uc_mcontext.gregs[10]);
#endif
    }

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

XR_FUNC void jit_debug_install_crash_handler(void) {
    if (getenv("XRAY_NO_JIT_CRASH_HANDLER")) {
        fprintf(stderr, "[JIT-debug] crash handler disabled by XRAY_NO_JIT_CRASH_HANDLER\n");
        return;
    }
    if (xr_proc_debugger_attached()) {
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

    xr_log_debug("jit", "crash handler installed (with alt stack)");
}

#else /* _WIN32 */

XR_FUNC void jit_debug_install_crash_handler(void) {
    /* Windows: signal-based crash handler not available.
     * TODO: implement via AddVectoredExceptionHandler. */
}

#endif /* _WIN32 */
