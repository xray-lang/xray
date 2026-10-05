/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 * test_backend_emission_resources.c - Explicit receiving ledger and host buffers
 */
#include "base/xmalloc.h"
#include "aot/program/xr_backend_ir.h"
#include "../program/xr_program_module_fixture.h"
#include "../plan/target_profile_test_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef XR_OS_WINDOWS
#include <windows.h>
#endif
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
typedef XrBackendStatus (*EmissionPrototype)(const XrBackendIR *, const XrBackendEmissionRequest *,
                                             XrGeneratedC *, XrBackendDiagnostic *);
_Static_assert(_Generic(&xr_compile_backend_ir_emit_c, EmissionPrototype: 1, default: 0),
               "One resource-bearing emission entry");
typedef struct PhysicalBlock {
    void *memory;
    size_t bytes;
} PhysicalBlock;
static PhysicalBlock physical[65536];
static const char *allocation_file;
static unsigned allocation_line;
XR_FUNC void backend_probe_site(const char *file, unsigned line) {
    allocation_file = file;
    allocation_line = line;
}
static size_t physical_count, physical_bytes, attempts, fail_at;
static const char *corrupt_expected_path;
static void corrupt_completed_emission(void) {
    if (!corrupt_expected_path || !allocation_file ||
        !strstr(allocation_file, "xcompile_resources.c"))
        return;
    size_t index = 0;
    for (size_t i = 1; i < physical_count; ++i)
        if (physical[i].bytes > physical[index].bytes)
            index = i;
    CHECK(physical_count && physical[index].bytes > 4096);
    char *source = physical[index].memory;
    size_t length = 0;
    while (length < physical[index].bytes && source[length])
        ++length;
    CHECK(length < physical[index].bytes);
    size_t close = length;
    while (close && source[close - 1] != '}')
        --close;
    CHECK(close);
    source[close - 1] = ' ';
    FILE *file = fopen(corrupt_expected_path, "wb");
    CHECK(file);
    CHECK(fwrite(source, 1, length, file) == length);
    CHECK(!fclose(file));
    corrupt_expected_path = NULL;
}
static size_t find_block(void *memory) {
    for (size_t i = 0; i < physical_count; ++i)
        if (physical[i].memory == memory)
            return i;
    CHECK(false);
    return 0;
}
XR_FUNC void *backend_probe_malloc(size_t bytes) {
    corrupt_completed_emission();
    if (++attempts == fail_at) {
        fprintf(stderr, "injected allocation %zu at %s:%u\n", attempts, allocation_file,
                allocation_line);
        return NULL;
    }
    void *memory = xr_malloc_raw(bytes);
    if (memory) {
        CHECK(physical_count < 65536);
        physical[physical_count++] = (PhysicalBlock) {memory, bytes};
        physical_bytes += bytes;
    }
    return memory;
}
XR_FUNC void *backend_probe_calloc(size_t count, size_t bytes) {
    if (bytes && count > SIZE_MAX / bytes)
        return NULL;
    void *memory = backend_probe_malloc(count * bytes);
    if (memory)
        memset(memory, 0, count * bytes);
    return memory;
}
XR_FUNC void *backend_probe_realloc(void *memory, size_t bytes) {
    if (++attempts == fail_at) {
        fprintf(stderr, "injected allocation %zu at %s:%u\n", attempts, allocation_file,
                allocation_line);
        return NULL;
    }
    size_t index = memory ? find_block(memory) : physical_count;
    size_t old_bytes = memory ? physical[index].bytes : 0;
    void *next = xr_realloc_raw(memory, bytes);
    if (next) {
        CHECK(index < 65536);
        if (!memory)
            ++physical_count;
        physical[index] = (PhysicalBlock) {next, bytes};
        physical_bytes = physical_bytes - old_bytes + bytes;
    }
    return next;
}
XR_FUNC void backend_probe_free(void *memory) {
    if (!memory)
        return;
    size_t index = find_block(memory);
    physical_bytes -= physical[index].bytes;
    physical[index] = physical[--physical_count];
    xr_free_raw(memory);
}
#include "xr_backend_emission_probe.h"
#include "../../../src/base/xcompile_resources.c"
#undef xr_calloc
#undef xr_malloc
#undef xr_realloc
#undef xr_free
static const XrBackendCExport probe_export = {0u, "emission_probe_init", 0u, 1u, {0, 0}};
static const XrCompileResourceLimits ample = {UINT64_C(8589934592), UINT64_C(16777216),
                                              UINT64_C(34359738368)};
static XrBackendIR *make_ir(void) {
    XrProgramModuleFixture fixture;
    xr_program_module_fixture_init(&fixture);
    XrCoreIrProgram *core = NULL;
    XrProgramArtifact artifact = {0};
    XrValidatedProgram *program = NULL;
    CHECK(xr_core_ir_program_build(&fixture.input, &core, NULL, 0) == XR_PROGRAM_BUILD_OK);
    CHECK(xr_program_write(core, &artifact, NULL, 0) == XR_PROGRAM_BUILD_OK);
    CHECK(xr_program_validate(artifact.bytes, artifact.size, NULL, &program, NULL) ==
          XR_PROGRAM_VERIFY_OK);
    xr_program_artifact_free(&artifact);
    xr_core_ir_program_free(core);
    XrTargetProfile *profile =
        xr_test_target_profile_build(false, XR_TARGET_RUNTIME_PROFILE_HOSTED);
    CHECK(profile);
    XrBackendOptions options = xr_backend_default_options();
    XrBackendIR *ir = NULL;
    CHECK(xr_backend_ir_build(program, profile, &options, &ir, NULL) == XR_BACKEND_OK);
    xr_validated_program_free(program);
    xr_target_profile_free(profile);
    return ir;
}
static void finish(XrCompileResources *resources, XrGeneratedC *output) {
    xr_generated_c_free(output);
    xr_compile_resources_release(resources);
    CHECK(!physical_count && !physical_bytes);
}
static XrBackendStatus probe(const XrBackendIR *ir, XrCompileResourceLimits limits, size_t fail,
                             XrCompileResourceStats *stats, size_t *sites) {
    CHECK(!physical_count && !physical_bytes);
    attempts = 0;
    fail_at = fail;
    XrCompileResources *resources = NULL;
    XrGeneratedC output = {0}, empty = {0};
    XrCompileResourceStatus created = xr_compile_resources_new(&limits, &resources);
    XrBackendStatus status = XR_BACKEND_OUT_OF_MEMORY;
    if (created == XR_COMPILE_RESOURCE_OK) {
        XrBackendEmissionRequest request = {resources, true, &probe_export, 1};
        status = xr_compile_backend_ir_emit_c(ir, &request, &output, NULL);
        if (status != XR_BACKEND_OK)
            CHECK(!memcmp(&output, &empty, sizeof(output)));
        CHECK(xr_compile_resources_stats(resources, stats) == XR_COMPILE_RESOURCE_OK);
    } else {
        CHECK(!resources);
        CHECK(created == XR_COMPILE_RESOURCE_OUT_OF_MEMORY ||
              created == XR_COMPILE_RESOURCE_BUDGET);
    }
    *sites = attempts;
    finish(resources, &output);
    fail_at = 0;
    return status;
}
static void refusal(const XrBackendIR *ir) {
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
    XrBackendEmissionRequest request = {resources, true, &probe_export, 1};
    XrCompileResourceStats before, after;
    CHECK(xr_compile_resources_stats(resources, &before) == XR_COMPILE_RESOURCE_OK);
    XrGeneratedC output = {0}, snapshot;
    CHECK(xr_compile_backend_ir_emit_c(ir, NULL, &output, NULL) == XR_BACKEND_INVALID_INPUT);
    XrBackendEmissionRequest absent = {NULL, true, NULL, 0};
    CHECK(xr_compile_backend_ir_emit_c(ir, &absent, &output, NULL) == XR_BACKEND_INVALID_INPUT);
    CHECK(xr_compile_backend_ir_emit_c(ir, &request, NULL, NULL) == XR_BACKEND_INVALID_INPUT);
    for (size_t i = 0; i < sizeof(output); ++i) {
        memset(&output, 0, sizeof(output));
        ((unsigned char *) &output)[i] = 1;
        snapshot = output;
        /* Padding is not an output member and does not make it occupied. */
        if (i >= offsetof(XrGeneratedC, execution_id) ||
            i < offsetof(XrGeneratedC, size) + sizeof(output.size) ||
            (i >= offsetof(XrGeneratedC, header_bytes) &&
             i < offsetof(XrGeneratedC, header_size) + sizeof(output.header_size))) {
            CHECK(xr_compile_backend_ir_emit_c(ir, &request, &output, NULL) ==
                  XR_BACKEND_INVALID_INPUT);
            CHECK(!memcmp(&output, &snapshot, sizeof(output)));
        }
    }
    CHECK(xr_compile_resources_stats(resources, &after) == XR_COMPILE_RESOURCE_OK);
    CHECK(!memcmp(&before, &after, sizeof(before)));
    memset(&output, 0, sizeof(output));
    finish(resources, &output);
}
static void boundaries(const XrBackendIR *ir, XrCompileResourceStats baseline) {
    for (unsigned axis = 0; axis < 3; ++axis) {
        XrCompileResourceLimits limits = ample;
        uint64_t *bound = axis == 0   ? &limits.allocated_bytes
                          : axis == 1 ? &limits.live_bytes
                                      : &limits.work;
        *bound = axis == 0   ? baseline.allocated_bytes
                 : axis == 1 ? baseline.peak_bytes
                             : baseline.work;
        XrCompileResourceStats stats = {0};
        size_t sites;
        CHECK(probe(ir, limits, 0, &stats, &sites) == XR_BACKEND_OK);
        --*bound;
        CHECK(probe(ir, limits, 0, &stats, &sites) == XR_BACKEND_RESOURCE_LIMIT);
    }
    XrCompileResourceLimits limits = ample;
    limits.work = baseline.work;
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&limits, &resources) == XR_COMPILE_RESOURCE_OK);
    XrBackendEmissionRequest request = {resources, true, &probe_export, 1};
    XrGeneratedC first = {0}, second = {0}, empty = {0};
    CHECK(xr_compile_backend_ir_emit_c(ir, &request, &first, NULL) == XR_BACKEND_OK);
    xr_generated_c_free(&first);
    CHECK(xr_compile_backend_ir_emit_c(ir, &request, &second, NULL) == XR_BACKEND_RESOURCE_LIMIT);
    CHECK(!memcmp(&second, &empty, sizeof(second)));
    finish(resources, &second);
}
static void write_native(const XrBackendIR *ir, const char *path, bool standalone_main) {
    XrCompileResources *resources = NULL;
    CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
    XrGeneratedC output = {0};
    XrBackendEmissionRequest request = {resources, standalone_main, &probe_export, 1};
    CHECK(xr_compile_backend_ir_emit_c(ir, &request, &output, NULL) == XR_BACKEND_OK);
    size_t path_bytes = strlen(path);
    CHECK(path_bytes <= SIZE_MAX - 12);
    char *header_path = backend_probe_malloc(path_bytes + 12);
    CHECK(header_path);
    memcpy(header_path, path, path_bytes);
    memcpy(header_path + path_bytes, ".h", 3);
    FILE *header = fopen(header_path, "wb");
    CHECK(header && output.header_bytes && output.header_size);
    CHECK(fwrite(output.header_bytes, 1, output.header_size, header) == output.header_size);
    CHECK(!fclose(header));
    const char *filename = path;
    for (const char *cursor = path; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\')
            filename = cursor + 1;
    FILE *file = fopen(path, "wb");
    CHECK(file);
    CHECK(fwrite(output.bytes, 1, output.size, file) == output.size);
    CHECK(!fclose(file));
    memcpy(header_path + path_bytes, ".consumer.c", 12);
    FILE *consumer = fopen(header_path, "wb");
    CHECK(consumer);
    CHECK(fprintf(consumer, "#include \"%s.h\"\n"
                            "int main(void) { emission_probe_init(); return 0; }\n",
                  filename) > 0);
    CHECK(!fclose(consumer));
    backend_probe_free(header_path);
    finish(resources, &output);
}
int main(int argc, char **argv) {
    XrBackendIR *ir = make_ir();
    if (argc == 3 && !strcmp(argv[1], "--ice")) {
#ifdef XR_OS_WINDOWS
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
        XrCompileResources *resources = NULL;
        CHECK(xr_compile_resources_new(&ample, &resources) == XR_COMPILE_RESOURCE_OK);
        XrGeneratedC output = {0};
        XrBackendEmissionRequest request = {resources, true, &probe_export, 1};
        /* Test-only allocator observation mutates one closing brace immediately
         * before the real verifier's first allocation. No production switch. */
        corrupt_expected_path = argv[2];
        (void) xr_compile_backend_ir_emit_c(ir, &request, &output, NULL);
        return 86;
    }
    if (argc == 2) {
        write_native(ir, argv[1], true);
        xr_backend_ir_free(ir);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--exports")) {
        write_native(ir, argv[2], false);
        xr_backend_ir_free(ir);
        return 0;
    }
    XrCompileResourceStats baseline = {0};
    size_t sites = 0;
    CHECK(probe(ir, ample, 0, &baseline, &sites) == XR_BACKEND_OK);
    CHECK(sites);
    for (size_t ordinal = 1; ordinal <= sites; ++ordinal) {
        XrCompileResourceStats stats = {0};
        size_t observed;
        XrBackendStatus status = probe(ir, ample, ordinal, &stats, &observed);
        if (status != XR_BACKEND_OUT_OF_MEMORY)
            fprintf(stderr, "fault ordinal=%zu status=%u attempts=%zu\n", ordinal,
                    (unsigned) status, observed);
        CHECK(status == XR_BACKEND_OUT_OF_MEMORY);
        CHECK(observed >= ordinal);
    }
    refusal(ir);
    boundaries(ir, baseline);
    xr_backend_ir_free(ir);
    printf("emission faults=%zu allocated=%llu peak=%llu work=%llu physical=0\n", sites,
           (unsigned long long) baseline.allocated_bytes, (unsigned long long) baseline.peak_bytes,
           (unsigned long long) baseline.work);
    return 0;
}
