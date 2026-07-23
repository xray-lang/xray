/*
 * test_xrd_effect_metadata.c - XRD signatures never carry textual effects
 */

#include "xanalyzer_xrd.h"
#include "../test_win_compat.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) static void test_##name(void)
#define RUN_TEST(name)                                                                             \
    do {                                                                                           \
        printf("  Running %s... ", #name);                                                         \
        test_##name();                                                                             \
        printf("PASSED\n");                                                                        \
        tests_passed++;                                                                            \
    } while (0)

#define ASSERT(cond)                                                                               \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond);                            \
            tests_failed++;                                                                        \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static bool write_temp_xrd(char path[128], const char *source) {
    strcpy(path, "/tmp/xray_xrd_signature_XXXXXX.xrd");
    int fd = xr_test_mkstemps(path, 4);
    if (fd < 0)
        return false;
    size_t size = strlen(source);
    bool ok = xr_test_write(fd, source, size) == (ssize_t) size;
    xr_test_close(fd);
    return ok;
}

static const XaBuiltinMember *find_member(const XaBuiltinMember *members, int count,
                                          const char *name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(members[i].name, name) == 0)
            return &members[i];
    }
    return NULL;
}

TEST(parses_signature_only_and_fails_closed) {
    const char *source = "export fn readText(path: string): string\n"
                         "export fn now(): int\n"
                         "type NativeBox = { const id: int }\n"
                         "fn NativeBox.run(): int\n";
    char path[128];
    ASSERT(write_temp_xrd(path, source));

    const XaBuiltinModule *module = xa_xrd_load_file(path);
    ASSERT(module != NULL);
    ASSERT(strcmp(xa_xrd_last_error(), "") == 0);

    const XaBuiltinMember *read_text =
        find_member(module->functions, module->function_count, "readText");
    const XaBuiltinMember *now = find_member(module->functions, module->function_count, "now");
    ASSERT(read_text != NULL);
    ASSERT(strcmp(read_text->signature, "(path: string): string") == 0);
    ASSERT(read_text->effect_contract.kind == XA_EFFECT_CONTRACT_MISSING);
    ASSERT(read_text->allocation_contract == XA_ALLOCATION_CONTRACT_MISSING);
    ASSERT(now != NULL);
    ASSERT(now->effect_contract.kind == XA_EFFECT_CONTRACT_MISSING);
    ASSERT(now->allocation_contract == XA_ALLOCATION_CONTRACT_MISSING);

    ASSERT(module->handle_count == 1);
    const XaBuiltinMember *run =
        find_member(module->handles[0].methods, module->handles[0].method_count, "run");
    ASSERT(run != NULL);
    ASSERT(run->effect_contract.kind == XA_EFFECT_CONTRACT_MISSING);
    ASSERT(run->allocation_contract == XA_ALLOCATION_CONTRACT_MISSING);

    xa_xrd_cleanup();
    xr_test_unlink(path);
}

TEST(rejects_every_textual_metadata_suffix) {
    static const char *const suffixes[] = {
        "@"
        "nothrow",
        "@"
        "errors(IoError.NotFound)",
        "@"
        "no_alloc",
        "@"
        "may_alloc",
    };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        char source[256];
        snprintf(source, sizeof(source), "export fn bad(): int %s\n", suffixes[i]);
        char path[128];
        ASSERT(write_temp_xrd(path, source));
        ASSERT(xa_xrd_load_file(path) == NULL);
        ASSERT(strstr(xa_xrd_last_error(), "XRD textual metadata is not supported") != NULL);
        xr_test_unlink(path);
    }
}

int main(void) {
    printf("Running XRD signature-only contract tests...\n");
    RUN_TEST(parses_signature_only_and_fails_closed);
    RUN_TEST(rejects_every_textual_metadata_suffix);

    printf("\n%d tests passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
