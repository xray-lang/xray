/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_cycle_batch_source_compiler.c - Finite Source closure graph and real VM/native preparation
 */
#define main cycle_compiler_original_main
#define p20_cell_cycle_fixture_program p20_cycle_batch_fixture_program
#define p20_cell_cycle_entries p20_cycle_batch_pair_entries
#include "test_xir_mixed_cell_cycles.c"
#undef p20_cell_cycle_entries
#undef p20_cell_cycle_fixture_program
#undef main
XR_DATA const uint32_t p20_cycle_batch_entries[3];
#include "xir_mixed_cycle_batch_source_prepare.h"

typedef struct CycleCompilerResult {
    XrXirStatus status;
    XrCompileResourceStats stats;
    unsigned stage;
    size_t sites, c_bytes;
    bool injected;
} CycleCompilerResult;

static XrCompileResourceLimits cb_compiler_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
}
static XrXirStatus cb_compiler_resource_status(XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return XR_XIR_OK;
    CHECK(status == XR_COMPILE_RESOURCE_BUDGET || status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
}
static bool cb_compiler_images(CycleSourcePrepare *source, CellCycleFixture *fixture, XrXirProgram **program) {
    const XrXirProgramSpec *native = &p20_cycle_batch_fixture_program;
    const XrXirModule *module = xr_xir_compile_artifact_module(source->lowered);
    CHECK(module->function_count == native->entry_count && module->function_count < 64);
    CHECK(!memcmp(source->entries, p20_cycle_batch_entries, sizeof(source->entries)));
    XrXirProgramProof proof = xr_xir_compile_program_proof(source->lowered);
    CHECK(proof.length == native->proof.length && !memcmp(proof.bytes, native->proof.bytes, proof.length));
    fixture->count = module->function_count;
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(source->context->resources, fixture->count,
        sizeof(*fixture->entries), (void **)&fixture->entries);
    if (!cb_source_stage(source, 9, cb_compiler_resource_status(allocated))) { CHECK(!fixture->entries); return false; }
    XrXirArtifact *original = source->lowered;
    XrXirStatus status = xr_xir_compile_vm_program_take(&source->lowered, &fixture->vm);
    if (!cb_source_stage(source, 10, status)) { CHECK(!fixture->vm && source->lowered == original); return false; }
    CHECK(!source->lowered && fixture->vm);
    unsigned vm = 0, c = 0;
    for (uint32_t f = 0; f < fixture->count; ++f) {
        fixture->root[f] = module->declarations->functions[f].module == module->declarations->root_module;
        bool native_entry = cycle_native(fixture->backend, fixture->root[f]);
        CHECK(native->entries[f].resume != fixture->vm->entries[f].resume);
        fixture->entries[f] = native_entry ? native->entries[f] : fixture->vm->entries[f];
        if (native_entry) ++c; else ++vm;
    }
    if (fixture->backend < 2) CHECK(vm && c);
    else CHECK(fixture->backend == 2 ? vm == fixture->count && !c : c == fixture->count && !vm);
    XrXirProgramSpec spec = *native;
    spec.entries = fixture->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){fixture, cycle_code_drop};
    status = xr_xir_compile_program_seal(source->context, &spec, program);
    if (!cb_source_stage(source, 11, status)) { CHECK(!*program && !fixture->releases); return false; }
    CHECK(*program && !fixture->releases); return true;
}
static void cb_compiler_product(const XrXirProgram *program) {
    CHECK(program && program->entry_count==15 && program->declarations->module_count==3 && !program->declarations->slot_count);
    for(unsigned i=0;i<3;++i){CHECK(p20_cycle_batch_entries[i]<program->entry_count && program->entries[p20_cycle_batch_entries[i]].parameter_count==(i?1u:2u));const XrXirTypeNode *type=xr_xir_callable_signature(program->types,program->entries[p20_cycle_batch_entries[i]].result);CHECK(type);}
}
static CycleCompilerResult cb_compiler_operation(unsigned backend, size_t failure,
    XrCompileResourceLimits caps, const char *dump) {
    CHECK(backend < 4 && !effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    CHECK(!effects_source_owner_count && caps.allocated_bytes <= 67108864 && caps.live_bytes <= 8388608 && caps.work <= 128000000);
    effects_compile_attempts = 0; effects_compile_fail_at = failure; effects_compile_injected = false;
    CycleCompilerResult result = {0}; XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    CycleSourcePrepare source = {.context=&context}; CellCycleFixture fixture = {.backend=backend}; XrXirProgram *program = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(&caps, &context.resources);
    if (cb_source_stage(&source, 1, cb_compiler_resource_status(created))) {
        fixture.resources = context.resources;
        if (cb_source_prepare(&source)) {
            result.c_bytes = source.generated.length;
            if (dump) {
                FILE *file = fopen(dump, "wb"); CHECK(file && fwrite(source.generated.text, 1, source.generated.length, file) == source.generated.length);
                CHECK(fprintf(file, "\nconst uint32_t p20_cycle_batch_entries[3] = {%uu,%uu,%uu};\nconst uint32_t p20_cycle_batch_pair_entries[2] = {%uu,%uu};\n", source.entries[0], source.entries[1], source.entries[2], source.entries[0], source.entries[1]) > 0 && !fclose(file));
            }
            xr_xir_compile_c_source_free(&source.generated);
            (void)cb_compiler_images(&source, &fixture, &program);
        }
    } else CHECK(!context.resources);
    result.status = source.status; result.stage = source.stage;
    cb_source_cleanup(&source);
    if (!program) {
        CHECK(result.status != XR_XIR_OK && !fixture.releases);
        xr_xir_compile_program_drop(fixture.vm); fixture.vm = NULL;
        xr_compile_resources_free(fixture.entries); fixture.entries = NULL;
    } else { CHECK(result.status == XR_XIR_OK && result.stage == 11); cb_compiler_product(program); }
    if (context.resources) CHECK(xr_compile_resources_stats(context.resources, &result.stats) == XR_COMPILE_RESOURCE_OK);
    size_t before_release = effects_compile_attempts;
    xr_compile_resources_release(context.resources); context.resources = NULL;
    if (program) {
        cb_compiler_product(program); xr_xir_compile_program_drop(program);
        CHECK(fixture.releases == 1 && !fixture.vm && !fixture.entries);
    }
    CHECK(effects_compile_attempts == before_release);
    result.sites = effects_compile_attempts; result.injected = effects_compile_injected;
    effects_compile_fail_at = SIZE_MAX;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes); return result;
}
static void cb_compiler_axes(unsigned backend, const CycleCompilerResult *normal) {
    const uint64_t exact[] = {normal->stats.allocated_bytes, normal->stats.peak_bytes, normal->stats.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        CHECK(exact[axis] > 1); XrCompileResourceLimits caps = cb_compiler_caps();
        if (!axis) caps.allocated_bytes = exact[axis]-minus;
        else if (axis == 1) caps.live_bytes = exact[axis]-minus;
        else caps.work = exact[axis]-minus;
        CycleCompilerResult result = cb_compiler_operation(backend, SIZE_MAX, caps, NULL);
        printf("Cycle Source compiler axis backend=%u axis=%u minus=%u status=%u stage=%u allocated=%llu peak=%llu work=%llu\n",
            backend, axis, minus, result.status, result.stage, (unsigned long long)result.stats.allocated_bytes,
            (unsigned long long)result.stats.peak_bytes, (unsigned long long)result.stats.work);
        CHECK(result.status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(result.stats.allocated_bytes <= caps.allocated_bytes && result.stats.peak_bytes <= caps.live_bytes && result.stats.work <= caps.work);
    }
}
static size_t cb_compiler_number(const char *text) {
    char *end = NULL; unsigned long number = strtoul(text, &end, 10);
    CHECK(text[0] && end && !*end && number < 20000); return (size_t)number;
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4 || argc == 6); (void)cycle_graph;(void)cycle_compiler_original_main;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4);
    CycleCompilerResult normal = cb_compiler_operation(backend, SIZE_MAX, cb_compiler_caps(), argc == 4 ? argv[3] : NULL);
    printf("Cycle Source compiler backend=%u status=%u stage=%u sites=%zu allocated=%llu peak=%llu work=%llu cbytes=%zu oneFiniteOwner SourceSessionDead independentPacketPoison realVMNative originalDelegates callerDead lease1 physical0/0\n",
        backend, normal.status, normal.stage, normal.sites, (unsigned long long)normal.stats.allocated_bytes,
        (unsigned long long)normal.stats.peak_bytes, (unsigned long long)normal.stats.work, normal.c_bytes); fflush(stdout);
    CHECK(normal.status == XR_XIR_OK && normal.stage == 11 && normal.sites && normal.sites < 20000);
    if (!strcmp(argv[2], "baseline")) { CHECK(argc == 3 || argc == 4); return 0; }
    if (!strcmp(argv[2], "axes")) { CHECK(argc == 3); cb_compiler_axes(backend, &normal); return 0; }
    CHECK(argc == 6 && !strcmp(argv[2], "fi"));
    size_t begin = cb_compiler_number(argv[3]), end = cb_compiler_number(argv[4]), expected = cb_compiler_number(argv[5]);
    CHECK(begin < end && end <= expected && normal.sites == expected);
    for (size_t site = begin; site < end; ++site) {
        CycleCompilerResult result = cb_compiler_operation(backend, site, cb_compiler_caps(), NULL);
        if (result.status != XR_XIR_OUT_OF_MEMORY) fprintf(stderr, "Cycle Source compiler FI backend=%u site=%zu stage=%u status=%u hit=%u attempts=%zu\n",
            backend, site, result.stage, result.status, (unsigned)result.injected, result.sites);
        CHECK(result.status == XR_XIR_OUT_OF_MEMORY && result.injected && result.sites > site);
    }
    printf("Cycle Source compiler complete FI backend=%u range=%zu:%zu of%zu everyOriginalOrdinal typedOOM physical0/0\n", backend, begin, end, expected);
    return 0;
}
