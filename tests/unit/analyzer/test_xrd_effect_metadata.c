/*
 * test_xrd_effect_metadata.c - Unit tests for canonical .xrd effect contracts
 */

#include "xanalyzer_xrd.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
    strcpy(path, "/tmp/xray_xrd_effect_XXXXXX.xrd");
    int fd = mkstemps(path, 4);
    if (fd < 0)
        return false;
    size_t size = strlen(source);
    bool ok = write(fd, source, size) == (ssize_t) size;
    close(fd);
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

TEST(parses_and_normalizes_effect_contracts) {
    const char *source = "export fn readText(path: string): string "
                         "@errors(IoError.PermissionDenied, IoError.NotFound)\n"
                         "export fn now(): int @nothrow\n"
                         "export fn unknown(): int\n"
                         "type NativeBox = { const id: int }\n"
                         "fn NativeBox.run(): int @errors(RunError.Failed)\n";
    char path[128];
    ASSERT(write_temp_xrd(path, source));

    const XaBuiltinModule *module = xa_xrd_load_file(path);
    ASSERT(module != NULL);
    ASSERT(strcmp(xa_xrd_last_error(), "") == 0);

    const XaBuiltinMember *read_text =
        find_member(module->functions, module->function_count, "readText");
    const XaBuiltinMember *now = find_member(module->functions, module->function_count, "now");
    const XaBuiltinMember *unknown =
        find_member(module->functions, module->function_count, "unknown");
    ASSERT(read_text != NULL);
    ASSERT(strcmp(read_text->signature, "(path: string): string") == 0);
    ASSERT(read_text->effect_contract.kind == XA_EFFECT_CONTRACT_ERRORS);
    ASSERT(read_text->effect_contract.error_count == 2);
    ASSERT(strcmp(read_text->effect_contract.errors[0], "IoError.NotFound") == 0);
    ASSERT(strcmp(read_text->effect_contract.errors[1], "IoError.PermissionDenied") == 0);
    ASSERT(now != NULL);
    ASSERT(strcmp(now->signature, "(): int") == 0);
    ASSERT(now->effect_contract.kind == XA_EFFECT_CONTRACT_NOTHROW);
    ASSERT(unknown != NULL);
    ASSERT(unknown->effect_contract.kind == XA_EFFECT_CONTRACT_MISSING);

    ASSERT(module->handle_count == 1);
    const XaBuiltinMember *run =
        find_member(module->handles[0].methods, module->handles[0].method_count, "run");
    ASSERT(run != NULL);
    ASSERT(strcmp(run->signature, "(): int") == 0);
    ASSERT(run->effect_contract.kind == XA_EFFECT_CONTRACT_ERRORS);
    ASSERT(run->effect_contract.error_count == 1);
    ASSERT(strcmp(run->effect_contract.errors[0], "RunError.Failed") == 0);

    xa_xrd_cleanup();
    unlink(path);
}

TEST(rejects_unknown_effect_metadata) {
    char path[128];
    ASSERT(write_temp_xrd(path, "export fn bad(): int @throws(IoError)\n"));
    ASSERT(xa_xrd_load_file(path) == NULL);
    ASSERT(strstr(xa_xrd_last_error(), "unknown effect metadata") != NULL);
    unlink(path);
}

TEST(rejects_duplicate_error_entries) {
    char path[128];
    ASSERT(
        write_temp_xrd(path, "export fn bad(): int @errors(IoError.NotFound, IoError.NotFound)\n"));
    ASSERT(xa_xrd_load_file(path) == NULL);
    ASSERT(strstr(xa_xrd_last_error(), "duplicate error") != NULL);
    unlink(path);
}

int main(void) {
    printf("Running .xrd effect metadata tests...\n");
    RUN_TEST(parses_and_normalizes_effect_contracts);
    RUN_TEST(rejects_unknown_effect_metadata);
    RUN_TEST(rejects_duplicate_error_entries);

    printf("\n%d tests passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
