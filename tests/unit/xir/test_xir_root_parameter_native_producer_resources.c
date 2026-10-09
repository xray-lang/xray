/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_native_producer_resources.c
 */
/* Resource qualification of the actual Source to CGen pipeline. */
#define main h1n_reference_producer_main
#include "test_xir_root_parameter_native_producer.c"
#undef main
#include <ctype.h>
#include "xir/xxir_program.h"
typedef struct H1ProducerFixture { uint32_t entry,run,module,slots; } H1ProducerFixture;
enum { H1C_OWNER, H1C_IO, H1C_SESSION, H1C_SOURCE, H1C_WRITE, H1C_READ,
    H1C_SPECIALIZE, H1C_CHECK, H1C_LOWER, H1C_LOWER_CHECK, H1C_SEAL, H1C_PHASES };
static const char *const h1c_names[H1C_PHASES] = {"owner", "source_io", "session", "Source", "Checked_write",
    "Checked_read", "specialize", "closed_reverify", "Lower", "Lowered_reverify", "CGen_W1_W4_publish"};
static const XrCompileResourceLimits h1c_caps = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
typedef struct H1Compiler {
    const RootParameterSourceOracle *oracle;
    XrXirCompileContext context;
    XrCompilerSession *session;
    XrXirArtifact *checked, *read, *closed, *lowered;
    XrXirCheckedPacket packet;
    H1ProducerFixture fixture;
    XrXirCSource generated;
    XrCompileResourceStats baseline;
    size_t sites[H1C_PHASES], owner_blocks, owner_bytes;
    unsigned phase;
    char path[1024];
} H1Compiler;

static void h1c_host_clear(H1Compiler *c) {
    XrOsIoPolicy host = xr_os_io_system_policy(); bool exists = false;
    XrOsIoStatus status = xr_os_io_exists(&host, c->path, &exists);
    CHECK(status == XR_OS_IO_OK || status == XR_OS_IO_NOT_FOUND);
    if (exists) CHECK(xr_os_io_remove(&host, c->path) == XR_OS_IO_OK);
}
static XrXirStatus h1c_io(XrOsIoStatus status) {
    if (status == XR_OS_IO_OK) return XR_XIR_OK;
    if (status == XR_OS_IO_OUT_OF_MEMORY) return XR_XIR_OUT_OF_MEMORY;
    if (status == XR_OS_IO_BUDGET) return XR_XIR_BUDGET;
    fprintf(stderr, "H1 producer unexpected IO=%u\n", (unsigned)status); CHECK(false); return XR_XIR_IO;
}
static uint64_t h1c_hash(const void *memory, size_t size) {
    const unsigned char *p = memory; uint64_t h = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < size; ++i) { h ^= p[i]; h *= UINT64_C(1099511628211); }
    return h;
}
static XrCompileResourceStats h1c_stats(H1Compiler *c) {
    XrCompileResourceStats s = {0};
    if (c->context.resources) CHECK(xr_compile_resources_stats(c->context.resources, &s) == XR_COMPILE_RESOURCE_OK);
    return s;
}
static void h1c_ledger(H1Compiler *c, const char *tag) {
    XrCompileResourceStats s = h1c_stats(c);
    printf("H1_NATIVE_PRODUCER_COMPILER_LEDGER file=%s tag=%s owner=%p attempts=%zu allocated=%llu live=%llu peak=%llu work=%llu "
        "physical_blocks=%zu physical_bytes=%zu\n", c->oracle->file, tag, (void *)c->context.resources,
        instance_compile_attempts, (unsigned long long)s.allocated_bytes, (unsigned long long)s.live_bytes,
        (unsigned long long)s.peak_bytes, (unsigned long long)s.work, instance_compile_live, instance_compile_bytes);
}
static XrXirStatus h1c_source(H1Compiler *c) {
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_ROOT_PARAMETER_NATIVE_FIXTURES};
    XrXirSourceRequest request = {c->session, c->path, &authority, &c->context, NULL, NULL, XR_XIR_PROGRAM, NULL};
    unsigned char before[sizeof(request)]; memcpy(before, &request, sizeof(request));
    XrXirSourceResult result = {0}, empty = {0};
    XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &failure);
    CHECK(!memcmp(&request, before, sizeof(request)) && diagnostic.status == status);
    if (status == XR_XIR_OK) { CHECK(result.checked && result.snapshot && !failure); c->checked = result.checked; result.checked = NULL; }
    else CHECK(!memcmp(&result, &empty, sizeof(result)) && !c->checked);
    xr_compile_resources_free(failure); xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(c->session); c->session = NULL;
    if (status != XR_XIR_OK) return status;
    XrOsIoPolicy io = xr_compile_io_policy(c->context.resources);
    return h1c_io(xr_os_io_remove(&io, c->path));
}
static XrXirStatus h1c_action(H1Compiler *c, unsigned p, const XrCompileResourceLimits *caps) {
    XrXirStatus status = XR_XIR_OK; const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    switch (p) {
    case H1C_OWNER: {
        XrCompileResourceStatus s = xr_compile_resources_new(caps, &c->context.resources);
        if (s != XR_COMPILE_RESOURCE_OK) {
            CHECK(!c->context.resources && (s == XR_COMPILE_RESOURCE_BUDGET || s == XR_COMPILE_RESOURCE_OUT_OF_MEMORY));
            return s == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
        }
        c->context.limits = xr_xir_compile_default_limits(); c->baseline = h1c_stats(c);
        c->owner_blocks = instance_compile_live; c->owner_bytes = instance_compile_bytes;
        CHECK(c->owner_blocks == 1 && c->baseline.live_bytes == sizeof(*c->context.resources)); break;
    }
    case H1C_IO: {
        XrOsIoPolicy io = xr_compile_io_policy(c->context.resources); bool exists = false;
        XrOsIoStatus s = xr_os_io_exists(&io, c->path, &exists);
        if (s != XR_OS_IO_NOT_FOUND && s != XR_OS_IO_OK) return h1c_io(s);
        CHECK(!exists); return h1c_io(xr_os_io_write_new_file_sync(&io, c->path,
            (const uint8_t *)c->oracle->text, strlen(c->oracle->text)));
    }
    case H1C_SESSION: {
        XrCompilerSessionStatus s = xr_compile_session_new(c->context.resources, &c->session);
        if (s != XR_COMPILER_SESSION_OK) {
            CHECK(!c->session && (s == XR_COMPILER_SESSION_BUDGET || s == XR_COMPILER_SESSION_OUT_OF_MEMORY));
            return s == XR_COMPILER_SESSION_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
        }
        CHECK(xr_compile_session_resources(c->session) == c->context.resources); break;
    }
    case H1C_SOURCE: return h1c_source(c);
    case H1C_WRITE:
        status = xr_xir_compile_checked_write(c->checked, &c->packet, NULL);
        if (status == XR_XIR_OK) { xr_xir_compile_artifact_free(c->checked); c->checked = NULL; }
        else CHECK(!c->packet.bytes && !c->packet.length); break;
    case H1C_READ:
        status = xr_xir_compile_checked_read(&c->context, c->packet.bytes, c->packet.length, &c->read, NULL);
        if (status == XR_XIR_OK) { memset(c->packet.bytes, 0xa5, c->packet.length); xr_xir_compile_checked_packet_free(&c->packet); }
        else CHECK(!c->read); break;
    case H1C_SPECIALIZE:
        status = xr_xir_compile_specialize(c->read, &c->closed, NULL);
        if (status == XR_XIR_OK) { xr_xir_compile_artifact_free(c->read); c->read = NULL; }
        else CHECK(!c->closed); break;
    case H1C_CHECK: {
        status = xr_xir_compile_artifact_verify(c->closed, NULL); if (status != XR_XIR_OK) break;
        const XrXirModule *m = xr_xir_compile_artifact_module(c->closed);
        CHECK(m->stage == XR_XIR_CHECKED && m->provenance && m->provenance->kind == XR_XIR_EVIDENCE_INSTANCE);
        c->fixture.run = h1n_export(m); XrXirEffects *effects = NULL;
        status = xr_xir_compile_effects_analyze(c->closed, &effects);
        if (status == XR_XIR_OK) {
            const XrXirRootEffects *f = xr_xir_effects_root(effects, c->fixture.run);
            CHECK(f && f->requires_root == c->oracle->root && f->unresolved == c->oracle->unresolved);
        } else CHECK(!effects);
        xr_xir_compile_effects_free(effects); break;
    }
    case H1C_LOWER:
        status = xr_xir_compile_lower(c->closed, &target, &c->lowered, NULL);
        if (status == XR_XIR_OK) { xr_xir_compile_artifact_free(c->closed); c->closed = NULL; }
        else CHECK(!c->lowered); break;
    case H1C_LOWER_CHECK: {
        status = xr_xir_compile_artifact_verify(c->lowered, NULL); if (status != XR_XIR_OK) break;
        const XrXirModule *m = xr_xir_compile_artifact_module(c->lowered); const XrXirDeclarations *d = m->declarations;
        CHECK(m->stage == XR_XIR_LOWERED && h1n_export(m) == c->fixture.run && d->module_count == 1);
        CHECK(d->entry_function < m->function_count && d->entry_function != c->fixture.run &&
            !m->functions[d->entry_function].parameter_count && m->functions[d->entry_function].result == XR_XIR_I64);
        CHECK(d->slot_count == (c->oracle->root ? 1u : 0u));
        if (d->slot_count) CHECK(d->slots[0].module == d->root_module && d->slots[0].mutable && d->slots[0].type == XR_XIR_I64);
        c->fixture.entry = d->entry_function; c->fixture.module = d->root_module; c->fixture.slots = d->slot_count; break;
    }
    case H1C_SEAL:
        status = xr_xir_compile_emit_c(c->lowered,"h1resource",4194304,&c->generated);
        if (status == XR_XIR_OK) {
            CHECK(c->generated.text && c->generated.length && !c->generated.text[c->generated.length]);
            CHECK(!strstr(c->generated.text,"({"));
            xr_xir_compile_artifact_free(c->lowered); c->lowered = NULL;
        } else CHECK(!c->generated.text && !c->generated.length);
        break;
    default: CHECK(false); break;
    }
    return status;
}
static XrXirStatus h1c_step(H1Compiler *c, unsigned p, const XrCompileResourceLimits *caps) {
    XrXirArtifact *input = p == H1C_WRITE ? c->checked : p == H1C_SPECIALIZE ? c->read :
        p == H1C_CHECK || p == H1C_LOWER ? c->closed : p >= H1C_LOWER_CHECK ? c->lowered : NULL;
    uint64_t hash = input ? h1c_hash(xr_xir_compile_artifact_module(input), sizeof(XrXirModule)) : 0;
    uint64_t packet_hash = p == H1C_READ ? h1c_hash(c->packet.bytes, c->packet.length) : 0;
    size_t begin = instance_compile_attempts; XrXirStatus status = h1c_action(c, p, caps);
    CHECK(instance_compile_attempts >= begin); c->sites[p] = instance_compile_attempts - begin; c->phase = p;
    if (status != XR_XIR_OK && input) CHECK(h1c_hash(xr_xir_compile_artifact_module(input), sizeof(XrXirModule)) == hash);
    if (status != XR_XIR_OK && p == H1C_READ) CHECK(h1c_hash(c->packet.bytes, c->packet.length) == packet_hash);
    printf("H1_NATIVE_PRODUCER_COMPILER_STAGE file=%s phase=%u name=%s first=%zu end=%zu sites=%zu status=%u\n",
        c->oracle->file, p, h1c_names[p], begin, instance_compile_attempts, c->sites[p], (unsigned)status);
    h1c_ledger(c, h1c_names[p]); return status;
}
static void h1c_init(H1Compiler *c, const RootParameterSourceOracle *oracle) {
    *c = (H1Compiler){0}; c->oracle = oracle;
    int n = snprintf(c->path, sizeof(c->path), "%s/%s", XR_ROOT_PARAMETER_NATIVE_FIXTURES, oracle->file);
    CHECK(n > 0 && (size_t)n < sizeof(c->path)); h1c_host_clear(c);
}
static void h1c_clear(H1Compiler *c) {
    XrCompileResourceStats before = h1c_stats(c);
    xr_xir_compile_c_source_free(&c->generated);
    xr_compile_session_free(c->session); c->session = NULL;
    xr_xir_compile_artifact_free(c->checked); c->checked = NULL;
    xr_xir_compile_artifact_free(c->read); c->read = NULL;
    xr_xir_compile_artifact_free(c->closed); c->closed = NULL;
    xr_xir_compile_artifact_free(c->lowered); c->lowered = NULL;
    xr_xir_compile_checked_packet_free(&c->packet); h1c_host_clear(c);
    XrCompileResourceStats after = h1c_stats(c);
    CHECK(after.allocated_bytes == before.allocated_bytes && after.work == before.work && after.peak_bytes == before.peak_bytes);
    if (c->context.resources) {
        CHECK(after.live_bytes == c->baseline.live_bytes && instance_compile_live == c->owner_blocks && instance_compile_bytes == c->owner_bytes);
    } else instance_compile_zero();
    h1c_ledger(c, "partial_free_fee_preserved");
}
static void h1c_drop(H1Compiler *c) {
    h1c_clear(c); xr_compile_resources_release(c->context.resources); c->context.resources = NULL;
    instance_compile_zero();
}
static XrXirStatus h1c_pipeline(H1Compiler *c, unsigned first, const XrCompileResourceLimits *caps) {
    for (unsigned p = first; p < H1C_PHASES; ++p) {
        XrXirStatus status = h1c_step(c, p, caps); if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static uint64_t h1_decimal(const char *text) {
    CHECK(text && *text); uint64_t value = 0;
    for (const char *p = text; *p; ++p) {
        CHECK(*p >= '0' && *p <= '9'); uint64_t digit = (uint64_t)(*p - '0');
        CHECK(value <= (UINT64_MAX - digit) / 10); value = value * 10 + digit;
    }
    return value;
}
static uint64_t h1_number(FILE *file) {
    char text[32] = {0}; CHECK(fscanf(file, "%31s", text) == 1); return h1_decimal(text);
}
static void h1_token(FILE *file, const char *expected) {
    char text[64] = {0}; CHECK(fscanf(file, "%63s", text) == 1 && !strcmp(text, expected));
}
static unsigned h1_unsigned(FILE *file) {
    uint64_t value = h1_number(file); CHECK(value <= UINT32_MAX); return (unsigned)value;
}
#include "xir_root_parameter_native_producer_resource_main.h"
