/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_root_parameter_vm.c - Real high-order Source execution after producer death
 *
 * KEY CONCEPT:
 *   One sealed program serves two independently metered VM instances.
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_output.h"
#include "toolchain/xcompiler_session.h"
#include "os/os_fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "H1 VM %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_instance_compile_observer.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "xir_root_parameter_vm_cases.h"

static void h1vm_ok(XrXirStatus status, const RootParameterSourceOracle *oracle, const char *phase) {
    if (status != XR_XIR_OK)
        fprintf(stderr, "H1 VM compiler file=%s phase=%s status=%u\n", oracle->file, phase, (unsigned)status);
    CHECK(status == XR_XIR_OK);
}

static uint32_t h1vm_export(const XrXirModule *module) {
    CHECK(module->declarations && module->declarations->functions);
    uint32_t found = UINT32_MAX;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *fn = &module->functions[f];
        const XrXirFunctionIdentity *id = &module->declarations->functions[f];
        if (fn->name_length != 3 || memcmp(fn->name, "run", 3) ||
            id->module != module->declarations->root_module || id->nominal_owner) continue;
        CHECK(found == UINT32_MAX && id->exported && !fn->parameter_count && fn->result == XR_XIR_I64);
        found = f;
    }
    CHECK(found != UINT32_MAX); return found;
}

enum { H1C_OWNER, H1C_IO, H1C_SESSION, H1C_SOURCE, H1C_WRITE, H1C_READ,
    H1C_SPECIALIZE, H1C_CHECK, H1C_LOWER, H1C_LOWER_CHECK, H1C_SEAL, H1C_PHASES };
static const char *const h1c_names[H1C_PHASES] = {"owner", "source_io", "session", "Source", "Checked_write",
    "Checked_read", "specialize", "closed_reverify", "Lower", "Lowered_reverify", "Program_seal"};
static const XrCompileResourceLimits h1c_caps = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
typedef struct H1Compiler {
    const RootParameterSourceOracle *oracle;
    XrXirCompileContext context;
    XrCompilerSession *session;
    XrXirArtifact *checked, *read, *closed, *lowered;
    XrXirCheckedPacket packet;
    H1VmFixture fixture;
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
    fprintf(stderr, "H1 VM unexpected IO=%u\n", (unsigned)status); CHECK(false); return XR_XIR_IO;
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
    printf("H1_VM_COMPILER_LEDGER file=%s tag=%s owner=%p attempts=%zu allocated=%llu live=%llu peak=%llu work=%llu "
        "physical_blocks=%zu physical_bytes=%zu\n", c->oracle->file, tag, (void *)c->context.resources,
        instance_compile_attempts, (unsigned long long)s.allocated_bytes, (unsigned long long)s.live_bytes,
        (unsigned long long)s.peak_bytes, (unsigned long long)s.work, instance_compile_live, instance_compile_bytes);
}
static XrXirStatus h1c_source(H1Compiler *c) {
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, XR_ROOT_PARAMETER_VM_FIXTURES};
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
        c->fixture.run = h1vm_export(m); XrXirEffects *effects = NULL;
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
        CHECK(m->stage == XR_XIR_LOWERED && h1vm_export(m) == c->fixture.run && d->module_count == 1);
        CHECK(d->entry_function < m->function_count && d->entry_function != c->fixture.run &&
            !m->functions[d->entry_function].parameter_count && m->functions[d->entry_function].result == XR_XIR_I64);
        CHECK(d->slot_count == (c->oracle->root ? 1u : 0u));
        if (d->slot_count) CHECK(d->slots[0].module == d->root_module && d->slots[0].mutable && d->slots[0].type == XR_XIR_I64);
        c->fixture.entry = d->entry_function; c->fixture.module = d->root_module; c->fixture.slots = d->slot_count; break;
    }
    case H1C_SEAL:
        status = xr_xir_compile_vm_program_take(&c->lowered, &c->fixture.program);
        if (status == XR_XIR_OK) {
            XrXirProgram *program = c->fixture.program; CHECK(program && !c->lowered && program->code.owner && program->code.release);
            CHECK(program->permissions && program->permissions->function_count == program->entry_count);
            const XrXirProgramPermission *f = &program->permissions->entries[c->fixture.run];
            CHECK(f->requires_root == c->oracle->root && f->unresolved == c->oracle->unresolved);
        } else CHECK(!c->fixture.program);
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
    printf("H1_VM_COMPILER_STAGE file=%s phase=%u name=%s first=%zu end=%zu sites=%zu status=%u\n",
        c->oracle->file, p, h1c_names[p], begin, instance_compile_attempts, c->sites[p], (unsigned)status);
    h1c_ledger(c, h1c_names[p]); return status;
}
static void h1c_init(H1Compiler *c, const RootParameterSourceOracle *oracle) {
    *c = (H1Compiler){0}; c->oracle = oracle;
    int n = snprintf(c->path, sizeof(c->path), "%s/%s", XR_ROOT_PARAMETER_VM_FIXTURES, oracle->file);
    CHECK(n > 0 && (size_t)n < sizeof(c->path)); h1c_host_clear(c);
}
static void h1c_clear(H1Compiler *c) {
    XrCompileResourceStats before = h1c_stats(c);
    xr_xir_compile_program_drop(c->fixture.program); c->fixture.program = NULL;
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
    CHECK(!runtime_live && !runtime_bytes); h1c_ledger(c, "partial_free_fee_preserved");
}
static void h1c_drop(H1Compiler *c) {
    h1c_clear(c); xr_compile_resources_release(c->context.resources); c->context.resources = NULL;
    instance_compile_zero(); CHECK(!runtime_live && !runtime_bytes);
}
static XrXirStatus h1c_pipeline(H1Compiler *c, unsigned first, const XrCompileResourceLimits *caps) {
    for (unsigned p = first; p < H1C_PHASES; ++p) {
        XrXirStatus status = h1c_step(c, p, caps); if (status != XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}
static H1VmFixture h1vm_build(const RootParameterSourceOracle *oracle) {
    CHECK(!runtime_live && !runtime_bytes); instance_compile_zero();
    CHECK(instance_compile_fail_at == SIZE_MAX && runtime_fail_at == SIZE_MAX);
    H1Compiler c; h1c_init(&c, oracle); h1vm_ok(h1c_pipeline(&c, 0, &h1c_caps), oracle, "complete_pipeline");
    xr_compile_resources_release(c.context.resources); c.context.resources = NULL;
    CHECK(instance_compile_live && instance_compile_bytes); return c.fixture;
}

static void h1vm_pair(const RootParameterSourceOracle *oracle, H1VmFixture fixture) {
    H1VmObservation o[2] = {{0}, {0}}; XrXirInstance *instances[2] = {NULL, NULL};
    XrXirDomain *domains[2] = {NULL, NULL};
    for (unsigned i = 0; i < 2; ++i) {
        o[i].oracle = oracle; o[i].module = fixture.module; o[i].slots = fixture.slots;
        o[i].sink = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION, 0, h1vm_bytes, &o[i], 65536};
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        config.metadata_limit = 65536; config.value_limit = 65536; config.call_limit = 65536;
        config.poll_limit = 8000; config.depth_limit = 96;
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, h1vm_typed, &o[i]};
        config.trace = h1vm_lifecycle; config.trace_context = &o[i];
        size_t begin = runtime_attempts;
        CHECK(xr_xir_instance_new(fixture.program, &config, &instances[i]) == XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_state(instances[i]) == XR_XIR_INSTANCE_NEW);
        o[i].instance = instances[i]; domains[i] = instances[i]->domain; CHECK(xr_xir_domain_retain(domains[i]));
        h1vm_runtime_report(oracle, "created", i, instances[i], runtime_attempts - begin);
    }
    CHECK(instances[0] != instances[1] && domains[0] != domains[1] &&
        &instances[0]->budget != &instances[1]->budget && instances[0]->program == instances[1]->program);
    xr_xir_compile_program_drop(fixture.program); fixture.program = NULL;
    for (unsigned i = 0; i < 2; ++i) {
        H1VmLedger peer = h1vm_ledger(instances[1 - i]);
        H1VmObservation peer_output = o[1 - i]; size_t begin = runtime_attempts;
        CHECK(xr_xir_instance_start(instances[i], fixture.entry, NULL, 0) == XR_XIR_CALL_READY);
        h1vm_unchanged(instances[1 - i], &peer); h1vm_output_unchanged(&o[1 - i], &peer_output);
        h1vm_runtime_report(oracle, "entry_start", i, instances[i], runtime_attempts - begin);
    }
    XrXirValue entry[2] = {{0}, {0}}, held[2] = {{0}, {0}}, aliases[2] = {{0}, {0}};
    h1vm_drive_pair(instances, o, entry, true);
    for (unsigned i = 0; i < 2; ++i) {
        xr_xir_value_drop(&entry[i]);
        CHECK(!entry[i].type && !entry[i].reserved && !entry[i].payload);
        CHECK(instances[i]->function_gate && o[i].published == fixture.slots);
        if (fixture.slots) h1vm_value(&instances[i]->slots[0], 9);
        H1VmLedger peer = h1vm_ledger(instances[1 - i]);
        H1VmObservation peer_output = o[1 - i]; size_t begin = runtime_attempts;
        CHECK(xr_xir_instance_start(instances[i], fixture.run, NULL, 0) == XR_XIR_CALL_READY);
        h1vm_unchanged(instances[1 - i], &peer); h1vm_output_unchanged(&o[1 - i], &peer_output);
        h1vm_runtime_report(oracle, "run_start", i, instances[i], runtime_attempts - begin);
    }
    CHECK(instances[0]->function_gate != instances[1]->function_gate);
    h1vm_drive_pair(instances, o, held, false);
    for (unsigned i = 0; i < 2; ++i) {
        H1VmLedger peer = h1vm_ledger(instances[1 - i]);
        CHECK(xr_xir_value_copy(&held[i], &aliases[i]) == XR_XIR_VALUE_OK);
        h1vm_value(&aliases[i], oracle->result); h1vm_unchanged(instances[1 - i], &peer);
    }
    H1VmLedger peer = h1vm_ledger(instances[1]); H1VmObservation peer_output = o[1];
    CHECK(xr_xir_instance_stop(instances[0]) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(instances[0]) == XR_XIR_CALL_READY); instances[0] = NULL;
    h1vm_domain_only(domains[0]); h1vm_unchanged(instances[1], &peer); h1vm_output_unchanged(&o[1], &peer_output);
    CHECK(o[0].released == fixture.slots && instance_compile_live && instance_compile_bytes);
    h1vm_value(&held[0], oracle->result);
    CHECK(xr_xir_instance_stop(instances[1]) == XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(instances[1]) == XR_XIR_CALL_READY); instances[1] = NULL;
    h1vm_domain_only(domains[1]); CHECK(o[1].released == fixture.slots); instance_compile_zero();
    size_t begin = runtime_attempts;
    for (unsigned i = 0; i < 2; ++i) {
        h1vm_output_done(&o[i]); h1vm_value(&held[i], oracle->result); h1vm_value(&aliases[i], oracle->result);
        xr_xir_value_drop(&held[i]); xr_xir_value_drop(&aliases[i]);
        CHECK(!held[i].type && !held[i].reserved && !held[i].payload &&
            !aliases[i].type && !aliases[i].reserved && !aliases[i].payload);
        printf("H1_VM_OUTPUT file=%s instance=%u typed_groups=%u typed_type=I64 value=%lld writes=%u bytes=",
            oracle->file, i, o[i].groups, (long long)oracle->result, o[i].writes);
        for (size_t b = 0; b < o[i].length; ++b) printf("%02x", (unsigned)(uint8_t)o[i].bytes[b]);
        printf(" begins=%u ready=%u published=%u released=%u\n", o[i].begins, o[i].ready, o[i].published, o[i].released);
        xr_xir_domain_drop(domains[i]); domains[i] = NULL;
    }
    CHECK(runtime_attempts == begin && !runtime_live && !runtime_bytes); instance_compile_zero();
    printf("H1_VM_PHYSICAL file=%s instances=2 compiler_blocks=0 compiler_bytes=0 runtime_blocks=0 runtime_bytes=0\n", oracle->file);
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
static H1Expected h1_oracle(const char *path, const RootParameterSourceOracle *oracle) {
    FILE *file = fopen(path, "rb"); CHECK(file); H1Expected e = {0};
    h1_token(file, "H1_VM_RESOURCE_R1"); CHECK(h1_number(file) == XR_XIR_CHECKED_SCHEMA);
    CHECK(h1_number(file) == XR_XIR_CHECKED_CONTRACT && h1_number(file) == XR_XIR_VALUE_ABI_VERSION);
    CHECK(h1_number(file) == XR_XIR_CALL_ABI_VERSION && h1_number(file) == XR_XIR_PROGRAM_ABI_VERSION);
    h1_token(file, oracle->file); h1_token(file, "compiler_sites");
    for (unsigned p = 0; p < H1C_PHASES; ++p) {
        uint64_t v = h1_number(file); CHECK((uint64_t)(size_t)v == v && v <= 65536); e.compiler_sites[p] = (size_t)v;
    }
    CHECK(e.compiler_sites[H1C_OWNER] == 1); h1_token(file, "compiler_axes");
    const uint64_t caps[3] = {h1c_caps.allocated_bytes, h1c_caps.live_bytes, h1c_caps.work};
    for (unsigned a = 0; a < 3; ++a) { e.compiler_axes[a] = h1_number(file); CHECK(e.compiler_axes[a] > 1 && e.compiler_axes[a] <= caps[a]); }
    const uint64_t limits[H1R_AXES] = {UINT64_C(67108864), UINT64_C(67108864), 65536, 65536, 65536, UINT64_C(128000000)};
    for (unsigned i = 0; i < 2; ++i) {
        h1_token(file, "instance"); CHECK(h1_number(file) == i); h1_token(file, "sites");
        for (unsigned p = 0; p < H1R_PHASES; ++p) {
            uint64_t v = h1_number(file); CHECK((uint64_t)(size_t)v == v && v <= 65536); e.sites[i][p] = (size_t)v;
        }
        CHECK(e.sites[i][H1R_CREATE] && e.sites[i][H1R_ENTRY_START] && e.sites[i][H1R_RUN_START]);
        CHECK(!e.sites[i][H1R_ENTRY_TAKE] && !e.sites[i][H1R_RUN_TAKE]);
        for (unsigned p = H1R_COPY; p < H1R_PHASES; ++p) CHECK(!e.sites[i][p]);
        h1_token(file, "selectors");
        for (unsigned a = 0; a < H1R_AXES; ++a) { e.axes[i][a] = h1_number(file); CHECK(e.axes[i][a] > 1 && e.axes[i][a] <= limits[a]); }
        h1_token(file, "minus_frontiers");
        for (unsigned a = 0; a < H1R_AXES; ++a) {
            H1Minus *m = &e.minus[i][a]; m->phase = h1_unsigned(file); m->state = h1_unsigned(file);
            m->groups = h1_unsigned(file); m->writes = h1_unsigned(file); m->exhausted = h1_unsigned(file);
            m->stop = h1_unsigned(file); m->free = h1_unsigned(file);
            m->retry_phase = h1_unsigned(file); m->retry_status = h1_unsigned(file);
            CHECK(m->phase <= H1R_FREE && m->phase != H1R_ENTRY_TAKE && m->phase != H1R_RUN_TAKE && m->phase != H1R_COPY);
            CHECK(m->state <= XR_XIR_INSTANCE_DRAINING && m->groups <= 1 && m->writes <= m->groups && m->exhausted <= 1);
            CHECK(m->stop == XR_XIR_CALL_READY || m->stop == XR_XIR_CALL_LIMIT);
            CHECK(m->free == XR_XIR_CALL_READY || m->free == XR_XIR_CALL_LIMIT);
            CHECK(m->retry_phase == H1R_PHASES || m->retry_phase == H1R_RUN_START || m->retry_phase == H1R_RUN_BODY);
            if (m->retry_phase != H1R_PHASES) CHECK(m->retry_status == XR_XIR_CALL_LIMIT || m->retry_status == XR_XIR_CALL_BAD_STATE);
        }
        h1_token(file, "same_quota"); H1Minus *q = &e.quota[i];
        q->phase = h1_unsigned(file); q->state = h1_unsigned(file); q->groups = h1_unsigned(file);
        q->writes = h1_unsigned(file); q->exhausted = h1_unsigned(file); q->stop = h1_unsigned(file); q->free = h1_unsigned(file);
        CHECK(q->phase >= H1R_RUN_START && q->phase <= H1R_FREE && q->state <= XR_XIR_INSTANCE_DRAINING);
        CHECK(q->groups == 1 && q->writes == 1 && q->exhausted <= 1);
        CHECK(q->stop == XR_XIR_CALL_READY || q->stop == XR_XIR_CALL_LIMIT);
        CHECK(q->free == XR_XIR_CALL_READY || q->free == XR_XIR_CALL_LIMIT);
        h1_token(file, "exact_close");
        for (unsigned a = 0; a < H1R_AXES; ++a) {
            e.exact_stop[i][a] = h1_unsigned(file); e.exact_free[i][a] = h1_unsigned(file);
            CHECK(e.exact_stop[i][a] == XR_XIR_CALL_READY && e.exact_free[i][a] == XR_XIR_CALL_READY);
        }
    }
    h1_token(file, "END"); int b; while ((b = fgetc(file)) != EOF) CHECK(isspace((unsigned char)b));
    CHECK(!ferror(file) && !fclose(file)); return e;
}
static void h1c_counts(const H1Compiler *c, const H1Expected *e) {
    XrCompileResourceStats s = {0}; CHECK(xr_compile_resources_stats(c->context.resources, &s) == XR_COMPILE_RESOURCE_OK);
    for (unsigned p = 0; p < H1C_PHASES; ++p) CHECK(c->sites[p] == e->compiler_sites[p]);
    CHECK(s.allocated_bytes == e->compiler_axes[0] && s.peak_bytes == e->compiler_axes[1] && s.work == e->compiler_axes[2]);
}
static void h1_census(const RootParameterSourceOracle *oracle, const H1Expected *e) {
    H1Compiler c; h1c_init(&c, oracle); h1vm_ok(h1c_pipeline(&c, 0, &h1c_caps), oracle, "census_compile");
    if (e) h1c_counts(&c, e);
    XrCompileResourceStats stats = h1c_stats(&c);
    printf("H1_VM_COMPILER_CENSUS file=%s sites=", oracle->file);
    for (unsigned p = 0; p < H1C_PHASES; ++p) printf("%s%zu", p ? "," : "", c.sites[p]);
    printf(" allocated=%llu peak=%llu work=%llu\n", (unsigned long long)stats.allocated_bytes,
        (unsigned long long)stats.peak_bytes, (unsigned long long)stats.work);
    xr_compile_resources_release(c.context.resources); c.context.resources = NULL;
    H1Runtime t; h1r_normal(&t, c.fixture, oracle);
    if (e) for (unsigned i = 0; i < 2; ++i) {
        for (unsigned p = 0; p < H1R_PHASES; ++p) CHECK(t.w[i].sites[p] == e->sites[i][p]);
        for (unsigned a = 0; a < H1R_AXES; ++a) CHECK(t.w[i].selectors[a] == e->axes[i][a]);
    }
}
static void h1c_oom(const RootParameterSourceOracle *oracle, const H1Expected *e, unsigned stage, size_t first, size_t count) {
    CHECK(stage < H1C_PHASES && count <= 64 && first <= e->compiler_sites[stage] && count <= e->compiler_sites[stage] - first);
    for (size_t point = first; point < first + count; ++point) {
        H1Compiler c; h1c_init(&c, oracle);
        for (unsigned p = 0; p < stage; ++p) CHECK(h1c_step(&c, p, &h1c_caps) == XR_XIR_OK);
        XrCompileResources *owner = c.context.resources; XrCompileResourceStats s0 = h1c_stats(&c); h1c_ledger(&c, "S0");
        size_t begin = instance_compile_attempts; CHECK(point < SIZE_MAX - begin);
        instance_compile_injected = false; instance_compile_fail_at = begin + point;
        XrXirStatus status = h1c_step(&c, stage, &h1c_caps); instance_compile_fail_at = SIZE_MAX;
        CHECK(status == XR_XIR_OUT_OF_MEMORY && instance_compile_injected && instance_compile_attempts == begin + point + 1);
        XrCompileResourceStats failed = h1c_stats(&c); h1c_clear(&c); h1c_ledger(&c, "S1");
        CHECK(c.context.resources == owner && failed.allocated_bytes >= s0.allocated_bytes && failed.work >= s0.work);
        if (owner) CHECK(failed.work > s0.work);
        CHECK(!c.fixture.program && !c.session && !c.checked && !c.read && !c.closed && !c.lowered && !c.packet.bytes);
        h1vm_ok(h1c_pipeline(&c, stage == H1C_OWNER ? 0 : H1C_IO, &h1c_caps), oracle, "same_owner_retry");
        XrCompileResourceStats s2 = h1c_stats(&c); h1c_ledger(&c, "S2");
        if (owner) CHECK(c.context.resources == owner && s2.allocated_bytes >= failed.allocated_bytes && s2.work >= failed.work);
        printf("H1_VM_COMPILER_RETRY phase=%u same_owner=%u refund=0\n", stage, (unsigned)(owner != NULL));
        xr_compile_resources_release(c.context.resources); c.context.resources = NULL;
        h1vm_pair(oracle, c.fixture);
        printf("H1_VM_COMPILER_OOM file=%s phase=%u point=%zu denominator=%zu physical=0/0,0/0\n",
            oracle->file, stage, point, e->compiler_sites[stage]);
    }
    printf("H1_VM_COMPILER_RANGE file=%s phase=%u first=%zu count=%zu denominator=%zu complete=1\n",
        oracle->file, stage, first, count, e->compiler_sites[stage]);
}
static void h1c_axes(const RootParameterSourceOracle *oracle, const H1Expected *e) {
    for (unsigned a = 0; a < 3; ++a) for (unsigned minus = 0; minus < 2; ++minus) {
        XrCompileResourceLimits caps = h1c_caps; uint64_t amount = e->compiler_axes[a] - minus;
        if (!a) caps.allocated_bytes = amount; else if (a == 1) caps.live_bytes = amount; else caps.work = amount;
        H1Compiler c; h1c_init(&c, oracle); XrXirStatus status = h1c_pipeline(&c, 0, &caps);
        CHECK(status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        if (!minus) {
            h1c_counts(&c, e); xr_compile_resources_release(c.context.resources); c.context.resources = NULL;
            h1vm_pair(oracle, c.fixture);
        } else {
            XrCompileResources *owner = c.context.resources; h1c_ledger(&c, "axis_S1"); h1c_clear(&c);
            XrCompileResourceStats before = h1c_stats(&c);
            CHECK(h1c_pipeline(&c, owner ? H1C_IO : H1C_OWNER, &caps) == XR_XIR_BUDGET);
            CHECK(c.context.resources == owner); XrCompileResourceStats after = h1c_stats(&c);
            CHECK(after.allocated_bytes >= before.allocated_bytes && after.work >= before.work); h1c_ledger(&c, "axis_S2");
            h1c_drop(&c);
        }
        printf("H1_VM_COMPILER_AXIS file=%s axis=%u minus1=%u cap=%llu physical=0/0,0/0\n",
            oracle->file, a, minus, (unsigned long long)amount);
    }
}
static const RootParameterSourceOracle *h1_case(const char *name) {
    const RootParameterSourceOracle *selected = NULL;
    for (size_t i = 0; i < sizeof(rps_oracles) / sizeof(rps_oracles[0]); ++i)
        if (!strcmp(name, rps_oracles[i].file)) { CHECK(!selected); selected = &rps_oracles[i]; }
    CHECK(selected); return selected;
}
int main(int argc, char **argv) {
    CHECK(argc >= 2 && argc <= 8);
    if (argc == 2) { const RootParameterSourceOracle *oracle = h1_case(argv[1]); h1vm_pair(oracle, h1vm_build(oracle)); return 0; }
    const RootParameterSourceOracle *oracle = h1_case(argv[2]);
    if (!strcmp(argv[1], "--census")) { CHECK(argc == 3); h1_census(oracle, NULL); return 0; }
    if (!strcmp(argv[1], "--typed-failure")) {
        CHECK(argc == 4 && (!strcmp(argv[3], "error") || !strcmp(argv[3], "limit")));
        h1r_typed_failure(oracle, !strcmp(argv[3], "limit")); return 0;
    }
    CHECK(argc >= 4); H1Expected e = h1_oracle(argv[3], oracle);
    if (!strcmp(argv[1], "--strict")) { CHECK(argc == 4); h1_census(oracle, &e); return 0; }
    /* Every fault or boundary mode qualifies the external denominator first. */
    h1_census(oracle, &e);
    if (!strcmp(argv[1], "--compiler-axes")) { CHECK(argc == 4); h1c_axes(oracle, &e); }
    else if (!strcmp(argv[1], "--runtime-axes")) { CHECK(argc == 4); h1r_axes(oracle, &e); }
    else if (!strcmp(argv[1], "--same-quota")) { CHECK(argc == 4); h1r_same_quota(oracle, &e); }
    else if (!strcmp(argv[1], "--compiler-oom")) {
        CHECK(argc == 7); uint64_t stage = h1_decimal(argv[4]), first = h1_decimal(argv[5]), count = h1_decimal(argv[6]);
        CHECK(stage < H1C_PHASES && (uint64_t)(size_t)first == first && count <= 64); h1c_oom(oracle, &e, (unsigned)stage, (size_t)first, (size_t)count);
    } else if (!strcmp(argv[1], "--runtime-oom")) {
        CHECK(argc == 8); uint64_t target = h1_decimal(argv[4]), phase = h1_decimal(argv[5]), first = h1_decimal(argv[6]), count = h1_decimal(argv[7]);
        CHECK(target < 2 && phase < H1R_PHASES && (uint64_t)(size_t)first == first && count <= 64);
        h1r_oom(oracle, &e, (unsigned)target, (unsigned)phase, (size_t)first, (size_t)count);
    } else CHECK(false);
    return 0;
}
