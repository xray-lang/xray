/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_mixed_task_source_compiler.c - Finite Source and genuine compound Task image preparation
 */
#define XR_TASK_RESULT_RESOURCE_MAIN
#include "test_xir_mixed_task_result_lifetime.c"
#include "xir_mixed_task_source_prepare.h"

typedef struct TaskCompilerResult {
    XrXirStatus status;
    XrCompileResourceStats stats;
    unsigned stage;
    size_t sites, c_bytes;
    bool injected;
} TaskCompilerResult;

static XrCompileResourceLimits task_compiler_caps(void) {
    return (XrCompileResourceLimits){UINT64_C(67108864), UINT64_C(8388608), UINT64_C(128000000)};
}
static XrXirStatus task_compiler_resource_status(XrCompileResourceStatus status) {
    if (status == XR_COMPILE_RESOURCE_OK) return XR_XIR_OK;
    CHECK(status == XR_COMPILE_RESOURCE_BUDGET || status == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    return status == XR_COMPILE_RESOURCE_BUDGET ? XR_XIR_BUDGET : XR_XIR_OUT_OF_MEMORY;
}
static bool task_compiler_images(TaskSourcePrepare *source, TaskLateFixture *fixture, XrXirProgram **program) {
    const XrXirProgramSpec *native = &p20_task_result_fixture_program;
    const XrXirModule *module = xr_xir_compile_artifact_module(source->lowered);
    CHECK(module->function_count == native->entry_count && module->function_count < 64);
    CHECK(!memcmp(source->entries, p20_task_result_entries, sizeof(source->entries)));
    XrXirProgramProof proof = xr_xir_compile_program_proof(source->lowered);
    CHECK(proof.length == native->proof.length && !memcmp(proof.bytes, native->proof.bytes, proof.length));
    fixture->count = module->function_count;
    XrCompileResourceStatus allocated = xr_compile_resources_calloc(source->context->resources, fixture->count,
        sizeof(*fixture->entries), (void **)&fixture->entries);
    if (!task_source_stage(source, 9, task_compiler_resource_status(allocated))) { CHECK(!fixture->entries); return false; }
    XrXirArtifact *original = source->lowered;
    XrXirStatus status = xr_xir_compile_vm_program_take(&source->lowered, &fixture->vm);
    if (!task_source_stage(source, 10, status)) { CHECK(!fixture->vm && source->lowered == original); return false; }
    CHECK(!source->lowered && fixture->vm);
    unsigned vm = 0, c = 0;
    for (uint32_t f = 0; f < fixture->count; ++f) {
        fixture->root[f] = module->declarations->functions[f].module == module->declarations->root_module;
        bool native_entry = task_late_native(fixture->backend, fixture->root[f]);
        CHECK(native->entries[f].resume != fixture->vm->entries[f].resume);
        fixture->entries[f] = native_entry ? native->entries[f] : fixture->vm->entries[f];
        if (native_entry) ++c; else ++vm;
    }
    if (fixture->backend < 2) CHECK(vm && c);
    else CHECK(fixture->backend == 2 ? vm == fixture->count && !c : c == fixture->count && !vm);
    XrXirProgramSpec spec = *native;
    spec.entries = fixture->entries; spec.types = module->types; spec.declarations = module->declarations;
    spec.proof = proof; spec.code = (XrXirCodeLease){fixture, task_late_code_free};
    status = xr_xir_compile_program_seal(source->context, &spec, program);
    if (!task_source_stage(source, 11, status)) { CHECK(!*program && !fixture->releases); return false; }
    CHECK(*program && !fixture->releases); return true;
}
#if XR_MIXED_TASK_RESULT_MODULES == 3
static void task_compiler_error_metadata(const XrXirProgram *program) {
    const XrXirDeclarations *d = program->declarations;
    const char *core = "memory-module-v1:id=23:xray-core-assertions-v1";
    CHECK(d->module_count == 3 && !d->root_module && d->slot_count == 1 && !d->slots[0].module);
    CHECK(d->modules[2].name_length == strlen(core) && !memcmp(d->modules[2].name, core, strlen(core)));
    const XrXirTypes *types = program->types;
    CHECK(types->nominals && types->nominals->identities && !types->nominals->declarations);
    uint32_t declaration = UINT32_MAX;
    for (uint32_t n = 0; n < types->nominals->count; ++n) {
        const XrXirNominalIdentity *identity = &types->nominals->identities[n];
        if (identity->name.length != 9 || memcmp(identity->name.bytes, "TaskFault", 9)) continue;
        CHECK(declaration == UINT32_MAX && identity->kind == XR_XIR_NOMINAL_ENUM && identity->exported);
        CHECK(identity->arity == 0 && identity->field_count == 1 && identity->variant_count == 1);
        CHECK(identity->variants[0].name.length == 6 && !memcmp(identity->variants[0].name.bytes, "Failed", 6));
        CHECK(!identity->variants[0].field_begin && identity->variants[0].field_count == 1);
        CHECK(identity->fields[0].name.length == 7 && !memcmp(identity->fields[0].name.bytes, "message", 7));
        declaration = n;
    }
    CHECK(declaration != UINT32_MAX); unsigned closed = 0;
    for (uint32_t n = 0; n < types->count; ++n) {
        const XrXirTypeNode *node = &types->nodes[n];
        if (node->kind != XR_XIR_TYPE_NOMINAL || node->nominal.declaration != declaration || node->parameter_span) continue;
        CHECK(node->nominal.field_count == 1 && node->nominal.fields && node->nominal.fields[0] == XR_XIR_STRING); ++closed;
    }
    CHECK(closed == 1);
}
#endif
static void task_compiler_product(const XrXirProgram *program) {
    CHECK(program && program->entry_count > p20_task_result_entries[2]);
    const XrXirTypeNode *task = xr_xir_type_node(program->types, program->entries[p20_task_result_entries[0]].result);
    CHECK(task && task->kind == XR_XIR_TYPE_TASK && !task->parameter_span);
    const XrXirTypeNode *array = xr_xir_type_node(program->types, task->element);
    CHECK(array && array->kind == XR_XIR_TYPE_ARRAY && array->element == XR_XIR_STRING && !array->parameter_span);
    CHECK(program->entries[p20_task_result_entries[2]].result == task->element);
#if XR_MIXED_TASK_RESULT_MODULES == 3
    task_compiler_error_metadata(program);
#endif
}
static TaskCompilerResult task_compiler_operation(unsigned backend, size_t failure,
    XrCompileResourceLimits caps, const char *dump) {
    CHECK(backend < 4 && !effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    CHECK(!effects_source_owner_count && caps.allocated_bytes <= 67108864 && caps.live_bytes <= 8388608 && caps.work <= 128000000);
    effects_compile_attempts = 0; effects_compile_fail_at = failure; effects_compile_injected = false;
    TaskCompilerResult result = {0}; XrXirCompileContext context = {0}; context.limits = xr_xir_compile_default_limits();
    TaskSourcePrepare source = {.context=&context}; TaskLateFixture fixture = {.backend=backend}; XrXirProgram *program = NULL;
    XrCompileResourceStatus created = xr_compile_resources_new(&caps, &context.resources);
    if (task_source_stage(&source, 1, task_compiler_resource_status(created))) {
        fixture.resources = context.resources;
        if (task_source_prepare(&source)) {
            result.c_bytes = source.generated.length;
            if (dump) {
                FILE *file = fopen(dump, "wb"); CHECK(file && fwrite(source.generated.text, 1, source.generated.length, file) == source.generated.length);
                CHECK(fprintf(file, "\nconst uint32_t p20_task_result_entries[3] = {%uu,%uu,%uu};\n", source.entries[0], source.entries[1], source.entries[2]) > 0 && !fclose(file));
            }
            xr_xir_compile_c_source_free(&source.generated);
            (void)task_compiler_images(&source, &fixture, &program);
        }
    } else CHECK(!context.resources);
    result.status = source.status; result.stage = source.stage;
    task_source_cleanup(&source);
    if (!program) {
        CHECK(result.status != XR_XIR_OK && !fixture.releases);
        xr_xir_compile_program_drop(fixture.vm); fixture.vm = NULL;
        xr_compile_resources_free(fixture.entries); fixture.entries = NULL;
    } else { CHECK(result.status == XR_XIR_OK && result.stage == 11); task_compiler_product(program); }
    if (context.resources) CHECK(xr_compile_resources_stats(context.resources, &result.stats) == XR_COMPILE_RESOURCE_OK);
    size_t before_release = effects_compile_attempts;
    xr_compile_resources_release(context.resources); context.resources = NULL;
    if (program) {
        task_compiler_product(program); xr_xir_compile_program_drop(program);
        CHECK(fixture.releases == 1 && !fixture.vm && !fixture.entries);
    }
    CHECK(effects_compile_attempts == before_release);
    result.sites = effects_compile_attempts; result.injected = effects_compile_injected;
    effects_compile_fail_at = SIZE_MAX;
    CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes); return result;
}
static void task_compiler_axes(unsigned backend, const TaskCompilerResult *normal) {
    const uint64_t exact[] = {normal->stats.allocated_bytes, normal->stats.peak_bytes, normal->stats.work};
    for (unsigned axis = 0; axis < 3; ++axis) for (unsigned minus = 0; minus < 2; ++minus) {
        CHECK(exact[axis] > 1); XrCompileResourceLimits caps = task_compiler_caps();
        if (!axis) caps.allocated_bytes = exact[axis]-minus;
        else if (axis == 1) caps.live_bytes = exact[axis]-minus;
        else caps.work = exact[axis]-minus;
        TaskCompilerResult result = task_compiler_operation(backend, SIZE_MAX, caps, NULL);
        printf("Task Source compiler axis backend=%u axis=%u minus=%u status=%u stage=%u allocated=%llu peak=%llu work=%llu\n",
            backend, axis, minus, result.status, result.stage, (unsigned long long)result.stats.allocated_bytes,
            (unsigned long long)result.stats.peak_bytes, (unsigned long long)result.stats.work);
        CHECK(result.status == (minus ? XR_XIR_BUDGET : XR_XIR_OK));
        CHECK(result.stats.allocated_bytes <= caps.allocated_bytes && result.stats.peak_bytes <= caps.live_bytes && result.stats.work <= caps.work);
    }
}
static size_t task_compiler_number(const char *text) {
    char *end = NULL; unsigned long number = strtoul(text, &end, 10);
    CHECK(text[0] && end && !*end && number < 20000); return (size_t)number;
}
int main(int argc, char **argv) {
    CHECK(argc == 3 || argc == 4 || argc == 6); (void)task_late_graph;
    static const char *const names[] = {"0", "1", "vm", "native"}; unsigned backend = 0;
    while (backend < 4 && strcmp(argv[1], names[backend])) ++backend;
    CHECK(backend < 4);
    TaskCompilerResult normal = task_compiler_operation(backend, SIZE_MAX, task_compiler_caps(), argc == 4 ? argv[3] : NULL);
    printf("Task Source compiler backend=%u status=%u stage=%u sites=%zu allocated=%llu peak=%llu work=%llu cbytes=%zu oneFiniteOwner SourceSessionDead independentPacketPoison realVMNative originalDelegates callerDead lease1 physical0/0\n",
        backend, normal.status, normal.stage, normal.sites, (unsigned long long)normal.stats.allocated_bytes,
        (unsigned long long)normal.stats.peak_bytes, (unsigned long long)normal.stats.work, normal.c_bytes); fflush(stdout);
    CHECK(normal.status == XR_XIR_OK && normal.stage == 11 && normal.sites && normal.sites < 20000);
    if (!strcmp(argv[2], "baseline")) { CHECK(argc == 3 || argc == 4); return 0; }
    if (!strcmp(argv[2], "axes")) { CHECK(argc == 3); task_compiler_axes(backend, &normal); return 0; }
    CHECK(argc == 6 && !strcmp(argv[2], "fi"));
    size_t begin = task_compiler_number(argv[3]), end = task_compiler_number(argv[4]), expected = task_compiler_number(argv[5]);
    CHECK(begin < end && end <= expected && normal.sites == expected);
    for (size_t site = begin; site < end; ++site) {
        TaskCompilerResult result = task_compiler_operation(backend, site, task_compiler_caps(), NULL);
        if (result.status != XR_XIR_OUT_OF_MEMORY) fprintf(stderr, "Task Source compiler FI backend=%u site=%zu stage=%u status=%u hit=%u attempts=%zu\n",
            backend, site, result.stage, result.status, (unsigned)result.injected, result.sites);
        CHECK(result.status == XR_XIR_OUT_OF_MEMORY && result.injected && result.sites > site);
    }
    printf("Task Source compiler complete FI backend=%u range=%zu:%zu of%zu everyOriginalOrdinal typedOOM physical0/0\n", backend, begin, end, expected);
    return 0;
}
