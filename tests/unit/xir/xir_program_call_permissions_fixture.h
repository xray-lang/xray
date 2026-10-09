/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_program_call_permissions_fixture.h - Source-owned authenticated call targets
 *
 * KEY CONCEPT:
 *   Real checked bodies and bound VM entries survive source and packet destruction.
 */
#ifndef XIR_PROGRAM_CALL_PERMISSIONS_FIXTURE_H
#define XIR_PROGRAM_CALL_PERMISSIONS_FIXTURE_H

enum { PCE_INSTANCES = 2, PCE_MAX_FUNCTIONS = 16 };
typedef enum PceMode { PCE_NORMAL, PCE_REQUIRED, PCE_UNKNOWN, PCE_MIXED,
    PCE_ADVERTISED_UNKNOWN } PceMode;
enum { PCE_MAIN, PCE_RUN, PCE_WORKER, PCE_LEAF, PCE_ROOT_TARGET, PCE_UNKNOWN_TARGET,
    PCE_MIXED_TARGET, PCE_NAMES };
static const char *const pce_names[PCE_NAMES] = {
    "main", "run", "worker", "leaf", "required", "unknown", "mixed"};
static const char pce_source[] =
    "var state:i64=91\n"
    "fn leaf()->i64{print(42);return 42}\n"
    "export fn required()->i64{print(701);return state}\n"
    "export fn unknown()->i64{print(702);const f:fn()->i64=leaf;"
        "try{return f()}catch(e){return -702}}\n"
    "export fn mixed()->i64{print(703);const held=state;const f:fn()->i64=leaf;"
        "try{return held+f()}catch(e){return -703}}\n"
    "fn worker()->i64{return leaf()}\n"
    "export fn run()->i64{return await go worker()}\n"
    "fn main()->i64{return 0}\n";

typedef struct PceLedger {
    XrXirDomainStats values;
    XrXirDomainBudgetStats domain;
    uint64_t call_requested, call_live, call_allocations, call_frees, transitions, resumes;
    size_t runtime_attempts;
} PceLedger;

typedef struct PceWitness {
    XrXirInstance *instance;
    XrXirDomain *domain;
    XrXirCall *root_call;
    XrXirOutputSink sink;
    PceLedger before_call, after_call, terminal;
    unsigned index, module_begins, module_ready, published, released;
    unsigned child_callbacks, call_requests, target_entered, leaf_entered;
    unsigned typed_groups, byte_writes, init_entered;
    unsigned entry_releases[PCE_MAX_FUNCTIONS];
    char bytes[64];
    size_t length;
    bool boundary_captured, boundary_checked, fee_preserved, slot_preserved;
    bool empty_preserved, occupied_preserved;
    XrXirValue borrowed, taken, advertised;
    unsigned carrier_created, carrier_dropped;
    XrXirValue slot_before;
    XrXirInstanceState finished_state;
    XrXirCallStatus outcome, take_empty, take_occupied, stop, freed;
} PceWitness;

typedef struct PceFixture {
    XrXirCompileContext compiler;
    XrCompileResourceStats compiler_baseline;
    XrXirArtifact *lowered;
    XrXirProgram *program;
    XrXirVmBinding bindings[PCE_MAX_FUNCTIONS];
    XrXirCallEntry original[PCE_MAX_FUNCTIONS];
    PceWitness witnesses[PCE_INSTANCES];
    uint32_t functions, entries[PCE_NAMES], entry, initializer, target, module;
    unsigned code_releases;
    XrXirType advertised_type;
    PceMode mode;
    char root[1024], file[1060];
} PceFixture;

static PceFixture *pce_current;
static XrXirAction pce_resume(XrXirCallView *view);
static void pce_release(XrXirCallView *view, XrXirCallStatus reason);

static void pce_code_release(void *opaque) {
    PceFixture *f = opaque;
    CHECK(f->lowered && !f->code_releases);
    ++f->code_releases;
    xr_xir_compile_artifact_free(f->lowered);
    f->lowered = NULL;
}

static uint32_t pce_function(const XrXirModule *m, const char *name) {
    uint32_t found = UINT32_MAX;
    size_t length = strlen(name);
    for (uint32_t i = 0; i < m->function_count; ++i)
        if (m->functions[i].name_length == length && !memcmp(m->functions[i].name, name, length)) {
            CHECK(found == UINT32_MAX && !m->functions[i].parameter_count && m->functions[i].result == XR_XIR_I64);
            found = i;
        }
    CHECK(found != UINT32_MAX);
    return found;
}

/* Find the actual explicit plain fn()->i64 advertisement already owned by
 * the unchanged Source's Lowered type pool; never fabricate a callable type. */
static XrXirType pce_advertised_unknown_type(const XrXirTypes *types) {
    CHECK(types && types->nodes);
    XrXirType found = XR_XIR_UNIT;
    for (uint32_t i = 0; i < types->count; ++i) {
        XrXirType type = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE + i);
        const XrXirTypeNode *node = xr_xir_callable_signature(types, type);
        if (node && !node->parameter_count && node->result == XR_XIR_I64 &&
            node->flags == XR_XIR_CALLABLE_ROOT_UNRESOLVED) {
            CHECK(found == XR_XIR_UNIT);
            found = type;
        }
    }
    CHECK(found != XR_XIR_UNIT);
    return found;
}

static void pce_source_check(PceFixture *f, XrXirArtifact **checked) {
    int n = snprintf(f->root, sizeof(f->root), "%s", XR_PROGRAM_CALL_SCRATCH);
    CHECK(n > 0 && (size_t)n < sizeof(f->root));
    n = snprintf(f->file, sizeof(f->file), "%s/root.xr", f->root);
    CHECK(n > 0 && (size_t)n < sizeof(f->file));
    XrOsIoPolicy io = xr_compile_io_policy(f->compiler.resources);
    CHECK(xr_os_io_mkdir(&io, f->root, 0700) == XR_OS_IO_OK);
    bool exists = false;
    XrOsIoStatus io_status = xr_os_io_exists(&io, f->file, &exists);
    CHECK(io_status == XR_OS_IO_OK || io_status == XR_OS_IO_NOT_FOUND);
    if (exists) CHECK(xr_os_io_remove(&io, f->file) == XR_OS_IO_OK);
    CHECK(xr_os_io_write_new_file_sync(&io, f->file, (const uint8_t *)pce_source,
        sizeof(pce_source) - 1) == XR_OS_IO_OK);
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(f->compiler.resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT, NULL, f->root};
    XrXirSourceRequest request = {session, f->file, &authority, &f->compiler, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirSourceResult result = {0};
    XrXirSourceDiagnostic diagnostic = {0};
    char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request, &result, &diagnostic, &failure);
    if (status != XR_XIR_OK)
        fprintf(stderr, "PCE Source status=%u line=%d column=%d message=%s\n",
            status, diagnostic.line, diagnostic.column, diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot && !failure);
    *checked = result.checked;
    result.checked = NULL;
    xr_xir_compile_source_result_free(&result);
    xr_compile_session_free(session);
    CHECK(xr_os_io_remove(&io, f->file) == XR_OS_IO_OK);
}

static void pce_prepare_lowered(PceFixture *f) {
    XrXirArtifact *checked = NULL, *decoded = NULL, *closed = NULL;
    pce_source_check(f, &checked);
    CHECK(xr_xir_compile_artifact_module(checked)->stage == XR_XIR_CHECKED);
    XrXirCheckedPacket packet = {0};
    CHECK(xr_xir_compile_checked_write(checked, &packet, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    CHECK(xr_xir_compile_checked_read(&f->compiler, packet.bytes, packet.length, &decoded, NULL) == XR_XIR_OK);
    memset(packet.bytes, 0xa5, packet.length);
    xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded, &closed, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(decoded);
    CHECK(xr_xir_compile_artifact_verify(closed, NULL) == XR_XIR_OK);
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed, &target, &f->lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_context(f->lowered)->resources == f->compiler.resources);
    CHECK(xr_xir_compile_artifact_module(f->lowered)->stage == XR_XIR_LOWERED);
}

static void pce_check_facts(const PceFixture *f) {
    const XrXirProgramPermissions *p = f->program->permissions;
    CHECK(p && p->function_count == f->functions && p->slot_count == 1);
    const uint32_t targets[4] = {f->entries[PCE_LEAF], f->entries[PCE_ROOT_TARGET],
        f->entries[PCE_UNKNOWN_TARGET], f->entries[PCE_MIXED_TARGET]};
    const bool expected[4][2] = {{false,false},{true,false},{false,true},{true,true}};
    for (unsigned i = 0; i < 4; ++i) {
        CHECK(p->entries[targets[i]].requires_root == expected[i][0]);
        CHECK(p->entries[targets[i]].unresolved == expected[i][1]);
    }
    CHECK(p->entries[f->initializer].requires_root && !p->entries[f->initializer].unresolved);
    CHECK(p->entries[f->entries[PCE_WORKER]].worker == XR_XIR_OK);
}

static void pce_build(PceFixture *f, PceMode mode) {
    CHECK(!pce_current);
    *f = (PceFixture){.mode = mode};
    pce_current = f;
    XrCompileResourceLimits caps = {UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
    CHECK(xr_compile_resources_new(&caps, &f->compiler.resources) == XR_COMPILE_RESOURCE_OK);
    f->compiler.limits = xr_xir_compile_default_limits();
    CHECK(xr_compile_resources_stats(f->compiler.resources, &f->compiler_baseline) == XR_COMPILE_RESOURCE_OK);
    pce_prepare_lowered(f);
    const XrXirModule *m = xr_xir_compile_artifact_module(f->lowered);
    CHECK(m->function_count <= PCE_MAX_FUNCTIONS && m->declarations->module_count == 1);
    CHECK(m->declarations->slot_count == 1 && m->declarations->slots[0].mutable == 1);
    CHECK(m->declarations->slots[0].type == XR_XIR_I64);
    f->functions = m->function_count;
    f->module = m->declarations->root_module;
    f->initializer = m->declarations->modules[f->module].initializer;
    for (unsigned n = 0; n < PCE_NAMES; ++n) f->entries[n] = pce_function(m, pce_names[n]);
    /* SCRIPT authority still builds a PROGRAM: its generated entry is $entry.
     * The private Source declaration named main is an ordinary function. */
    f->entry = m->declarations->entry_function;
    CHECK(f->entry == pce_function(m, "$entry"));
    CHECK(f->entry != f->entries[PCE_MAIN] && f->entry != f->initializer);
    CHECK(m->declarations->functions[f->entry].module == f->module &&
        !m->declarations->functions[f->entry].exported &&
        !m->declarations->functions[f->entries[PCE_MAIN]].exported);
    CHECK(m->declarations->functions[f->entries[PCE_RUN]].exported);
    if (mode == PCE_ADVERTISED_UNKNOWN) f->advertised_type = pce_advertised_unknown_type(m->types);
    f->target = f->entries[(mode == PCE_NORMAL || mode == PCE_ADVERTISED_UNKNOWN) ? PCE_LEAF : mode == PCE_REQUIRED ?
        PCE_ROOT_TARGET : mode == PCE_UNKNOWN ? PCE_UNKNOWN_TARGET : PCE_MIXED_TARGET];
    XrXirCallEntry entries[PCE_MAX_FUNCTIONS];
    for (uint32_t i = 0; i < f->functions; ++i) {
        CHECK(xr_xir_compile_vm_bind(f->lowered, i, &f->bindings[i], &f->original[i]) == XR_XIR_OK);
        entries[i] = f->original[i];
        entries[i].resume = pce_resume;
        entries[i].release = pce_release;
    }
    const XrXirTarget target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};
    const XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, target, entries, f->functions,
        m->declarations, {f,pce_code_release}, m->types, xr_xir_compile_program_proof(f->lowered)};
    CHECK(xr_xir_compile_program_seal(&f->compiler, &spec, &f->program) == XR_XIR_OK);
    pce_check_facts(f);
}
#endif // XIR_PROGRAM_CALL_PERMISSIONS_FIXTURE_H
