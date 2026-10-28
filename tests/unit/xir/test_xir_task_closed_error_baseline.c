/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_task_closed_error_baseline.c - Certified owned errors survive Task completion
 */
#include "xir/xxir_source.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_effects.h"
#include "xir/xxir_vm.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "Task closed Error FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#include "xir_effect_execution_owner.h"
#include "xir_runtime_allocations.h"
#include "xir/xxir_task.c"
#include "xir_task_closed_error_pipeline.h"
#include "xir/xxir_nullable.h"
#include "xir/xxir_tuple.h"
#include "xir/xxir_enum.h"
#include "xir/xxir_atomic.h"
#include "task_closed_error_generated.h"
#ifdef XR_TASK_STRUCT_FIXTURE
#include "xir_task_struct_expect.h"
#else
#include "xir_task_result_expect.h"
#endif

enum { CLOSED_ERROR_FUNCTIONS = 128 };
typedef struct ClosedErrorFixture {
    XrXirProgram *program;
    XrXirArtifact *lowered;
    XrXirCallEntry entries[CLOSED_ERROR_FUNCTIONS];
    XrXirVmBinding *bindings;
    uint32_t entry[CLOSED_ERROR_CASES][2];
    unsigned code_releases;
} ClosedErrorFixture;
static void closed_error_code_drop(void *owner) {
    ClosedErrorFixture *fixture = owner;
    CHECK(fixture->lowered && !fixture->code_releases++);
    xr_compile_resources_free(fixture->bindings);fixture->bindings=NULL;
    xr_xir_compile_artifact_free(fixture->lowered); fixture->lowered = NULL;
}
static void closed_error_build_configure(ClosedErrorFixture *fixture,const char *backend,
    void (*configure)(ClosedErrorFixture *,const XrXirModule *)) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    fixture->lowered = closed_error_lower(context, fixture->entry);
    const XrXirModule *module = xr_xir_compile_artifact_module(fixture->lowered);
    const XrXirProgramSpec *native = &task_closed_error_native_program;
    CHECK(module->function_count <= CLOSED_ERROR_FUNCTIONS && module->function_count == native->entry_count);
    XrXirProgramProof proof = xr_xir_compile_program_proof(fixture->lowered);
    CHECK(proof.length == native->proof.length && !memcmp(proof.bytes, native->proof.bytes, proof.length));
    bool vm = !strcmp(backend, "vm"), all_native = !strcmp(backend, "native");
    bool root_native = !strcmp(backend, "native-vm");
    CHECK(vm || all_native || root_native || !strcmp(backend, "vm-native"));
    XrXirCallEntry *vm_entries=NULL;
    CHECK(xr_xir_compile_vm_bind_table(fixture->lowered,&fixture->bindings,&vm_entries)==XR_XIR_OK);
    for (uint32_t f = 0; f < module->function_count; ++f) {
        bool worker = module->functions[f].name_length >= 7 && !memcmp(module->functions[f].name, "worker_", 7);
        bool use_native = all_native || (!vm && (worker != root_native));
        fixture->entries[f] = use_native ? native->entries[f] : vm_entries[f];
    }
    if (configure) configure(fixture,module);
    XrXirProgramSpec spec = {XR_XIR_PROGRAM_ABI_VERSION, native->target, fixture->entries, module->function_count,
        module->declarations, {fixture, closed_error_code_drop}, module->types, proof};
    CHECK(xr_xir_compile_program_seal(context, &spec, &fixture->program) == XR_XIR_OK);
    xr_compile_resources_free(vm_entries);
    for (unsigned n = 0; n < CLOSED_ERROR_CASES; ++n) for (unsigned mode = 0; mode < 2; ++mode)
        CHECK(fixture->entry[n][mode] == closed_error_entries[n][mode]);
}
static void closed_error_build(ClosedErrorFixture *fixture,const char *backend) {
    closed_error_build_configure(fixture,backend,NULL);
}
static void closed_error_task_field_source(void) {
    const XrXirCompileContext *context = effects_source_owner(UINT64_C(67108864), UINT64_C(128000000));
    XrCompilerSession *session = NULL;
    CHECK(xr_compile_session_new(context->resources, &session) == XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority = {XR_MODULE_IDENTITY_SCRIPT,NULL,XR_TASK_CLOSED_ERROR_FIXTURES};
    XrXirSourceRequest request = {session,XR_TASK_CLOSED_ERROR_FIXTURES "/task-field.xr",&authority,
        context,XR_SOURCE_STDLIB,NULL,XR_XIR_PROGRAM,NULL};
    XrXirSourceResult result = {0}; XrXirSourceDiagnostic diagnostic = {0}; char *failure = NULL;
    XrXirStatus status = xr_xir_compile_source_check(&request,&result,&diagnostic,&failure);
    if (status != XR_XIR_OK) fprintf(stderr,"Task field Source status=%u %s\n",status,diagnostic.message);
    CHECK(status == XR_XIR_OK && result.checked && result.snapshot && !failure);
    xr_compile_session_free(session);
    const XrXirModule *module=xr_xir_compile_artifact_module(result.checked);
    bool field=false;
    for (uint32_t n=0;n<module->types->nominals->count;++n) {
        const XrXirNominalDeclaration *nominal=&module->types->nominals->declarations[n];
        for (uint32_t f=0;f<nominal->field_count;++f) {
            const XrXirTypeNode *node=xr_xir_type_node(module->types,nominal->fields[f].type);
            if (node && node->kind==XR_XIR_TYPE_TASK && node->element==XR_XIR_I64) field=true;
        }
    }
    CHECK(field);
    XrXirCheckedPacket packet={0};XrXirArtifact *decoded=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_write(result.checked,&packet,NULL)==XR_XIR_OK);
    xr_compile_resources_free(failure); xr_xir_compile_source_result_free(&result);
    CHECK(xr_xir_compile_checked_read(context,packet.bytes,packet.length,&decoded,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(decoded,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(decoded);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    CHECK(xr_xir_compile_artifact_verify(lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(lowered);
    effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes && !runtime_live && !runtime_bytes);
    printf("TASK_ERROR_TASK_FIELD Source/Checked/Lowered directTaskField producerDeath physical=0/0\n");
}
static bool closed_error_value(const XrXirValue *error,unsigned family) {
    XrXirValue concrete=*error;
    if (error->type==XR_XIR_ERROR) {
        concrete=(XrXirValue){0};CHECK(xr_xir_error_borrow(error,&concrete));
    }
    XrXirEnumBorrow record={0};
    CHECK(xr_xir_enum_borrow(&concrete,&record)==XR_XIR_VALUE_OK);
#ifdef XR_TASK_STRUCT_FIXTURE
    const char *members[]={"Struct"};
#else
    const char *members[]={"Array","Nested","Nullable","NullableArray","Tuple","Enum","Atomic","TaskArray","Task"};
#endif
    CHECK(family<CLOSED_ERROR_CASES && record.name.length==7 && !memcmp(record.name.bytes,"Problem",7) &&
        record.member.length==strlen(members[family]) && !memcmp(record.member.bytes,members[family],record.member.length) && record.field_count==1);
    if (family==8) return constructed_value(&record.fields[0],7);
    if (family!=7) return constructed_value(&record.fields[0],family);
    const XirObject *object=(const XirObject *)(uintptr_t)record.fields[0].payload;
    XrXirValueAdmission admission={object->arena,object->domain,NULL,NULL,1000000,16777216};
    int64_t length=0;XrXirValueStatus status=xr_xir_array_len(&record.fields[0],&admission,&length);
    if (status!=XR_XIR_VALUE_OK) { CHECK(status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_OOM);return false; }
    CHECK(length==1);
    XrXirValue task={0};XrXirFaultDetail fault={0};
    status=xr_xir_array_get(&record.fields[0],0,&admission,&task,&fault);
    if (status!=XR_XIR_VALUE_OK) { CHECK(status==XR_XIR_VALUE_LIMIT || status==XR_XIR_VALUE_OOM);return false; }
    bool valid=constructed_value(&task,7);xr_xir_value_drop(&task);return valid;
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!strcmp(argv[1],"source-task-field")) { closed_error_task_field_source(); return 0; }
    CHECK(argc == 2); ClosedErrorFixture fixture = {0}; closed_error_build(&fixture, argv[1]);
    XrXirType problem = XR_XIR_UNIT;
    for (uint32_t t = 0; t < fixture.program->types->count; ++t) {
        const XrXirTypeNode *node = &fixture.program->types->nodes[t];
        if (node->kind != XR_XIR_TYPE_NOMINAL) continue;
        const XrXirNominalIdentity *identity = &fixture.program->types->nominals->identities[node->nominal.declaration];
        if (identity->name.length == 7 && !memcmp(identity->name.bytes, "Problem", 7)) {
            CHECK(problem == XR_XIR_UNIT && identity->variant_count == CLOSED_ERROR_CASES);
            problem = (XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+t);
        }
    }
    CHECK(problem != XR_XIR_UNIT);
    unsigned workers = 0;
    for (uint32_t f = 0; f < fixture.program->entry_count; ++f) {
        const XrXirModule *module = xr_xir_compile_artifact_module(fixture.lowered);
        if (module->functions[f].name_length < 7 || memcmp(module->functions[f].name, "worker_", 7)) continue;
        CHECK(fixture.program->permissions->entries[f].worker == XR_XIR_OK);
        const XrXirEffects *effects = fixture.program->permissions->effects;
        CHECK(effects && !xr_xir_effects_error_unknown(effects, f) && !xr_xir_effects_error_unidentified(effects, f));
        uint64_t query_work=0;
        XrCompileResourceStats before_query={0},after_query={0};
        CHECK(xr_compile_resources_stats(fixture.program->context.resources,&before_query)==XR_COMPILE_RESOURCE_OK);
        CHECK(xr_xir_effects_error_query_work(effects,f,&query_work)==XR_XIR_OK && query_work>=2+3*CLOSED_ERROR_CASES);
        CHECK(xr_compile_resources_stats(fixture.program->context.resources,&after_query)==XR_COMPILE_RESOURCE_OK &&
            !memcmp(&before_query,&after_query,sizeof(before_query)));
        unsigned family = CLOSED_ERROR_CASES;
        for (unsigned n = 0; n < CLOSED_ERROR_CASES; ++n)
            if (module->functions[f].name_length == 7+strlen(closed_error_names[n]) &&
                !memcmp(module->functions[f].name+7, closed_error_names[n], strlen(closed_error_names[n]))) family = n;
        CHECK(family < CLOSED_ERROR_CASES);
        for (unsigned v = 0; v < CLOSED_ERROR_CASES; ++v)
            CHECK(xr_xir_effects_error(effects, f, problem, v) == (family == v));
        ++workers;
    }
    CHECK(workers == CLOSED_ERROR_CASES);
    for (unsigned n = 0; n < CLOSED_ERROR_CASES; ++n) for (unsigned mode=0;mode<2;++mode) {
        XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
        XrXirInstance *instance = NULL;
        CHECK(xr_xir_instance_new(fixture.program, &config, &instance) == XR_XIR_CALL_READY);
        bool last=n+1==CLOSED_ERROR_CASES && mode==1;
        if (last) { xr_xir_compile_program_drop(fixture.program);fixture.program=NULL; }
        CHECK(xr_xir_instance_start(instance, fixture.entry[n][mode], NULL, 0) == XR_XIR_CALL_READY);
        unsigned polls = 0; XrXirValue task = {0};
        while (!instance->executor->active) {
            CHECK(xr_xir_instance_poll_bounded(instance, 1).outcome.status == XR_XIR_CALL_READY && ++polls < 4096);
        }
        XrXirValue handle = task_core_task_value(instance->executor->active);
        CHECK(xr_xir_value_copy(&handle, &task) == XR_XIR_VALUE_OK);
        XrXirInstanceResult result = {0};
        do { result = xr_xir_instance_poll_bounded(instance, 1); CHECK(++polls < 4096); }
        while (result.outcome.status == XR_XIR_CALL_READY);
        CHECK(result.outcome.status == (mode?XR_XIR_CALL_RETURNED:XR_XIR_CALL_THROWN));
        XrXirValue root={0};
        CHECK(xr_xir_instance_take_result(instance,&root)==result.outcome.status);
        if (!mode) CHECK(closed_error_value(&root,n));
        xr_xir_value_drop(&root);
        CHECK(xr_xir_instance_free(instance) == XR_XIR_CALL_READY);
        if (last) CHECK(fixture.code_releases==1 && !fixture.lowered);
        XrXirCallResult first={0},second={0};
        CHECK(xr_xir_task_copy_outcome(&task,&first)==XR_XIR_CALL_THROWN);
        CHECK(xr_xir_task_copy_outcome(&task,&second)==XR_XIR_CALL_THROWN);
        CHECK(closed_error_value(&first.value,n) && closed_error_value(&second.value,n));
        xr_xir_call_result_drop(&first);xr_xir_call_result_drop(&second);xr_xir_value_drop(&task);
        CHECK(!runtime_live && !runtime_bytes);
    }
    CHECK(!fixture.program && fixture.code_releases == 1 && !fixture.lowered);
    effects_source_owners_free(); CHECK(!effects_compile_live && !effects_compile_bytes);
    printf("TASK_CLOSED_ERROR %s workers=%u rootOperations=%u THROWN typedPayload stickyCopies2 lastCodeLease1 physical=0/0\n",argv[1],workers,2*CLOSED_ERROR_CASES);
    return 0;
}
