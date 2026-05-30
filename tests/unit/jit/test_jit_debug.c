#include "xm_jit_debug.h"
#include "os_codemem.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_arm64_mcinsn_name(void) {
    const char *name = jit_debug_disasm_mcinsn_name(JIT_DEBUG_DISASM_ARM64, 0x8b020020u);
    assert(name != NULL);
    assert(strcmp(name, "arm64.add.rrr") == 0);
}

static void test_riscv64_mcinsn_name(void) {
    const char *name = jit_debug_disasm_mcinsn_name(JIT_DEBUG_DISASM_RISCV64, 0x002080b3u);
    assert(name != NULL);
    assert(strcmp(name, "riscv64.add") == 0);
}

static void test_unknown_mcinsn_name(void) {
    char buf[64];
    int n = jit_debug_format_mcinsn(buf, sizeof(buf), JIT_DEBUG_DISASM_ARM64, 0xffffffffu);
    assert(n > 0);
    assert(strcmp(buf, "(unknown)") == 0);
}

static void test_guard_page_fault_addr_range(void) {
    size_t page_size = xr_os_page_size();
    char page[32768];
    assert(page_size > 0);
    assert(page_size <= sizeof(page) / 2);
    void *base = page + page_size;
    assert(jit_debug_is_guard_page_fault_addr(base, base));
    assert(jit_debug_is_guard_page_fault_addr((char *) base + 17, base));
    assert(jit_debug_is_guard_page_fault_addr((char *) base + page_size - 1, base));
    assert(!jit_debug_is_guard_page_fault_addr((char *) base - 1, base));
    assert(!jit_debug_is_guard_page_fault_addr((char *) base + page_size, base));
    assert(!jit_debug_is_guard_page_fault_addr(NULL, base));
    assert(!jit_debug_is_guard_page_fault_addr(base, NULL));
}

int main(void) {
    test_arm64_mcinsn_name();
    test_riscv64_mcinsn_name();
    test_unknown_mcinsn_name();
    test_guard_page_fault_addr_range();
    fprintf(stderr, "jit debug tests passed\n");
    return 0;
}
