/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_allocations.c - Allocation failure and physical release witnesses
 *
 * KEY CONCEPT:
 *   Compile the actual implementation against counted allocator wrappers and
 *   fail each allocation in turn, including post-transition verification.
 */

#include "xir_construction_fixture.h"
#include "base/xmalloc.h"
#include "xir/xxir.h"
#include <stdio.h>
#include <stdlib.h>

static const XrXirTarget fixture_target = {XR_XIR_ARCH_X86_64, XR_XIR_VALUE_ABI_VERSION};

static size_t allocation_calls, live, class_live_bytes, fail_at = SIZE_MAX;
static const char *allocation_case_name = "selection", *allocation_stage = "arguments";
static unsigned allocation_case_scalar, allocation_case_mode;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "allocation check failed at line %d: %s\n", __LINE__, #condition); \
        fprintf(stderr, "family=%s scalar=%u mode=%u stage=%s calls=%zu fault_enabled=%u fault_ordinal=%zu\n", \
            allocation_case_name, allocation_case_scalar, allocation_case_mode, allocation_stage, \
            allocation_calls, (unsigned)(fail_at != SIZE_MAX), fail_at == SIZE_MAX ? 0 : fail_at); \
        exit(1); \
    } \
} while (0)

typedef struct AllocationRecord { void *pointer; size_t bytes; uint64_t identity; } AllocationRecord;
#include "xir_allocation_request_trace.h"
static AllocationRecord *allocation_records;
static size_t allocation_capacity;
#include "xir_segment_lifetime_probe.h"
/* Bookkeeping is outside the counted implementation allocation boundary. */
static void allocation_record(void *pointer, size_t bytes) {
    if (!pointer) return;
    if (live == allocation_capacity) {
        CHECK(allocation_capacity <= SIZE_MAX / 2 / sizeof(*allocation_records));
        size_t capacity=allocation_capacity ? allocation_capacity*2 : 256;
        AllocationRecord *grown=xr_realloc(allocation_records,capacity*sizeof(*grown));
        CHECK(grown);allocation_records=grown;allocation_capacity=capacity;
    }
    CHECK(bytes<=SIZE_MAX-class_live_bytes);
    allocation_records[live++]=(AllocationRecord){pointer,bytes,new_allocation_identity()};class_live_bytes+=bytes;
}
static void *counted_malloc(size_t size) {
    allocation_request_record(0,size,0,false);
    if (allocation_calls++ == fail_at) return NULL;
    void *pointer=xr_malloc(size);allocation_record(pointer,size);return pointer;
}
static void *counted_calloc(size_t count, size_t size) {
    allocation_request_record(1,count,size,false);
    if (allocation_calls++ == fail_at) return NULL;
    CHECK(!size || count<=SIZE_MAX/size);
    void *pointer=xr_calloc(count,size);allocation_record(pointer,count*size);return pointer;
}
static void counted_free(void *pointer) {
    uint64_t identity=0;
    if (pointer) {
        size_t index=0;while(index<live && allocation_records[index].pointer!=pointer)++index;
        CHECK(index<live);identity=allocation_records[index].identity;
        class_live_bytes-=allocation_records[index].bytes;
        allocation_records[index]=allocation_records[--live];
    }
    xr_free(pointer);
    if (identity) segment_probe_free(identity);
    if (!live) {xr_free(allocation_records);allocation_records=NULL;allocation_capacity=0;CHECK(!class_live_bytes);}
}
static void *counted_realloc(void *pointer, size_t size) {
    allocation_request_record(2,size,0,pointer==NULL);
    if (allocation_calls++ == fail_at) return NULL;
    size_t index=0;while(index<live && allocation_records[index].pointer!=pointer)++index;
    CHECK(!pointer || index<live);
    bool was_null=pointer==NULL;
    void *replacement=xr_realloc(pointer,size);
    if (replacement) {
        if (!was_null) {
            class_live_bytes-=allocation_records[index].bytes;
            CHECK(size<=SIZE_MAX-class_live_bytes);class_live_bytes+=size;
            uint64_t identity=allocation_records[index].identity;
            if (replacement!=pointer) {segment_probe_free(identity);identity=new_allocation_identity();}
            allocation_records[index]=(AllocationRecord){replacement,size,identity};
        } else allocation_record(replacement,size);
    }
    return replacement;
}

#include "xir_allocation_bookkeeping_cases.h"

#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_realloc
#define xr_malloc(size) counted_malloc(size)
#define xr_calloc(count, size) counted_calloc(count, size)
#define xr_free(pointer) counted_free(pointer)
#define xr_realloc(pointer, size) counted_realloc(pointer, size)

#include "base/xcompile_resources.c"
#include "xir/xxir_types.c"
#include "xir/xxir_constraints.c"
#include "xir/xxir_constraint_proof.c"
#include "xir/xxir_implementation.c"
#include "xir/xxir_implementation_verify.c"
#include "xir/xxir_interface.c"
#include "xir/xxir_interface_members.c"
#include "xir/xxir_type_layout.c"
#include "xir/xxir_generic.c"
#include "xir/xxir_defaults.c"
#include "xir/xxir_specialize.c"
#include "xir/xxir.c"
#include "xir/xxir_checked.c"
#include "base/xsha256.c"
#include "xir/xxir_declarations.c"
#include "xir/xxir_verify.c"
#include "xir/xxir_effects.c"
#include "xir/xxir_layout.c"
#include "xir/xxir_value.c"
#include "xir/xxir_type_arena.c"
#include "xir/xxir_scalar.c"
#include "xir/xxir_float.c"
#include "xir/xxir_vm.c"
#include "xir/xxir_emit_c.c"
#include "xir/xxir_call.c"
#include "xir/xxir_task_budget.c"
#include "xir/xxir_task.c"
#include "xir/xxir_program_match.c"
#include "xir/xxir_program.c"
static void string_instance_retiring(void *pointer);
static void allocation_instance_metadata_free(XrXirDomain *domain,void *pointer,uint64_t bytes);
#define xr_xir_domain_metadata_deallocate(domain,pointer,bytes) allocation_instance_metadata_free(domain,pointer,bytes)
#include "xir/xxir_instance.c"
#undef xr_xir_domain_metadata_deallocate
static void allocation_instance_metadata_free(XrXirDomain *domain,void *pointer,uint64_t bytes) {
    string_instance_retiring(pointer);
    xr_xir_domain_metadata_deallocate(domain,pointer,bytes);
}
#define XIR_STRING_INSTANCE_IMPLEMENTATION_PRESENT
#include "xir_string_instance_probe.c"
#undef XIR_STRING_INSTANCE_IMPLEMENTATION_PRESENT
#define XIR_INSTANCE_IMPLEMENTATION_PRESENT
#include "xir_instance_retirement_probe.c"
#undef XIR_INSTANCE_IMPLEMENTATION_PRESENT
#include "xir/xxir_output.c"

#include "xir_allocation_compile_owner.h"
#include "xir_string_fixture.h"
#include "xir_call_fixture.h"
#include "xir_local_fixture.h"
#include "xir_segment_cases.h"
#include "xir_function_cases.h"
#include "xir_function_producer_owner_cases.h"
#include "xir_invocation_runtime_owner_cases.h"
#include "xir_invocation_direct_owner_cases.h"
#include "xir_invocation_capture_owner_cases.h"
#include "xir_invocation_return_capture_owner_cases.h"
#include "xir_invocation_function_cell_owner_cases.h"
#include "xir_instance_admission_cases.h"
#include "xir_type_temporary_core_budget_cases.h"
#include "xir_constraint_temporary_core_cases.h"
#include "xir_access_temporary_cases.h"
#include "xir_initialization_allocations.h"
#include "xir_interface_packet_allocations.h"
#include "xir_constraint_packet_allocations.h"
#include "xir_implementation_allocations.h"
#include "xir_implementation_packet_allocations.h"
#include "xir_generic_method_allocations.h"
static void phi_snapshot_failure(void) {
    SourceFixtureOwner owner = {0}; source_fixture_owner_new(&owner);
    XrXirArtifact *checked = local_fixture(&owner.context), *lowered = NULL;
    size_t baseline = live, sites = 0;
    for (size_t attempt = 0; attempt <= sites; ++attempt) {
        allocation_calls = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
        XrXirStatus status = xr_xir_compile_lower(checked, &fixture_target, &lowered, NULL);
        if (!attempt) { CHECK(status == XR_XIR_OK); sites = allocation_calls; }
        else CHECK(status == XR_XIR_OUT_OF_MEMORY && !lowered);
        xr_xir_compile_artifact_free(lowered); lowered = NULL;
        CHECK(live == baseline);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_compile_lower(checked, &fixture_target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    const XrXirModule *module = xr_xir_compile_artifact_module(lowered);
    const XrXirFunctionLayout *layout = xr_xir_compile_artifact_layout(lowered, 2);
    void *frame = xr_calloc(1, layout->frame_bytes); CHECK(frame);
    XrXirDomain *domain = NULL; XrXirValue a = {0}, b = {0};
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "a", 1, &a) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "b", 1, &b) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_owned_slot_copy(frame, layout->offsets[0], NULL, XR_XIR_STRING, a.payload) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_owned_slot_copy(frame, layout->offsets[1], NULL, XR_XIR_STRING, b.payload) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_owned_slot_copy(frame, layout->offsets[6], NULL, XR_XIR_STRING, b.payload) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_owned_slot_copy(frame, layout->offsets[7], NULL, XR_XIR_STRING, a.payload) == XR_XIR_VALUE_OK);
    XirObject *object = object_pointer(&b);
    uint32_t references = atomic_load(&object->references);
    atomic_store(&object->references, UINT32_MAX);
    ScalarRun run = {.module=module, .function=&module->functions[2], .layout=layout,
        .frame=frame, .view=NULL, .compile_resources=owner.context.resources};
    CHECK(scalar_edge(&run, 2, 1) == XR_XIR_RUN_FRAME_LIMIT);
    CHECK(xr_xir_scalar_load(frame, layout->offsets[6]) == b.payload);
    CHECK(xr_xir_scalar_load(frame, layout->offsets[7]) == a.payload);
    CHECK(xr_xir_scalar_load(frame, layout->offsets[6] + 8) == a.payload);
    CHECK(xr_xir_scalar_load(frame, layout->offsets[7] + 8) == 0);
    atomic_store(&object->references, references);
    for (uint32_t i = layout->owned_count; i > 0; --i) xr_xir_owned_slot_clear(frame, layout->owned_offsets[i - 1]);
    CHECK(atomic_load(&object_pointer(&a)->references) == 1 && atomic_load(&object_pointer(&b)->references) == 1);
    xr_free(frame); xr_xir_value_drop(&a); xr_xir_value_drop(&b); xr_xir_domain_drop(domain);
    xr_xir_compile_artifact_free(lowered); source_fixture_owner_free(&owner); CHECK(!live);
    printf("PHI lower physical release: %zu allocation sites; partial snapshot failure leaves destinations unchanged\n", sites);
}

static XrXirOutputStatus allocation_bytes(void *context, XrXirOutputStream stream, const char *bytes, size_t length) {
    size_t *published = context;
    CHECK(stream == XR_XIR_STDERR && length == 1 && bytes[0] == 'x');
    ++*published; return XR_XIR_OUTPUT_OK;
}
static void allocation_instance_config(XrXirInstanceConfig *config) {
    CHECK(xr_xir_instance_config_init(config,sizeof(*config))==XR_XIR_CALL_READY);
    config->metadata_limit=65536;config->value_limit=65536;config->call_limit=65536;
    config->poll_limit=100;config->depth_limit=10;config->work_limit=1000000;
}
static XrXirStatus allocation_write_program(const XrXirCompileContext *context,XrXirProgram **output) {
    const XrXirType parameter=XR_XIR_STRING;
    const XrXirInstruction init={.op=XR_XIR_RETURN};
    const XrXirInstruction write[]={
        {.op=XR_XIR_WRITE_STREAM,.type=XR_XIR_BOOL,.args={0},.immediate=2},
        {.op=XR_XIR_RETURN,.args={1}}};
    const XrXirInstruction main[]={
        {.op=XR_XIR_CONST_INT,.type=XR_XIR_I64},
        {.op=XR_XIR_RETURN,.args={0}}};
    const XrXirBlock blocks[]={{.count=1},{.count=2},{.count=2}};
    const XrXirFunction functions[]={
        {.name="init",.name_length=4,.result=XR_XIR_UNIT,.blocks=&blocks[0],.block_count=1,
            .instructions=&init,.instruction_count=1},
        {.name="write",.name_length=5,.parameters=&parameter,.parameter_count=1,.result=XR_XIR_BOOL,
            .blocks=&blocks[1],.block_count=1,.instructions=write,.instruction_count=2},
        {.name="main",.name_length=4,.result=XR_XIR_I64,.blocks=&blocks[2],.block_count=1,
            .instructions=main,.instruction_count=2}};
    const XrXirSourceModule source={.name="output",.name_length=6,.initializer=0};
    const XrXirFunctionIdentity identities[]={{0},{.exported=1},{0}};
    const XrXirDeclarations declarations={.modules=&source,.module_count=1,.functions=identities,
        .root_module=0,.entry_function=2};
    const XrXirModule built={.stage=XR_XIR_BUILT,.functions=functions,.function_count=3,
        .declarations=&declarations,.linkage_kind=XR_XIR_PROGRAM};
    XrXirArtifact *checked=NULL,*lowered=NULL;
    XrXirStatus status=xir_fixture_check(context,&built,&checked,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_lower(checked,&fixture_target,&lowered,NULL);
    if(status==XR_XIR_OK)status=xr_xir_compile_vm_program_take(&lowered,output);
    if(status==XR_XIR_OK)CHECK(*output && !lowered);
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(checked);return status;
}
static size_t write_allocation_failures(void) {
    SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
    XrXirProgram *program=NULL;
    CHECK(allocation_write_program(&owner.context,&program)==XR_XIR_OK);
    XrXirDomain *domain=NULL;XrXirValue argument={0};
    CHECK(xr_xir_domain_new(65536,&domain)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain,"x",1,&argument)==XR_XIR_VALUE_OK);
    size_t baseline=live,sites=0;XrCompileResourceStats prefix=allocation_compile_stats(&owner.context);
    for(size_t attempt=0;attempt<=sites;++attempt) {
        size_t published=0;
        XrXirOutputSink sink={XR_XIR_CALL_ABI_VERSION,0,allocation_bytes,&published,65536};
        XrXirInstanceConfig config;allocation_instance_config(&config);
        config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,xr_xir_output_render,&sink};
        XrXirInstance *instance=NULL;XrXirDomain *runtime_domain=NULL;
        XrXirCallStatus retirement=XR_XIR_CALL_READY;
        allocation_calls=0;fail_at=attempt?attempt-1:SIZE_MAX;
        XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);
        if(status==XR_XIR_CALL_READY) {
            runtime_domain=xr_string_instance_domain(instance);
            status=xr_xir_instance_start(instance,1,&argument,1);
            if(status==XR_XIR_CALL_READY) {
                XrXirCallResult result=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome;
                if(attempt) {
                    CHECK(result.status==XR_XIR_CALL_OOM && result.value.type==XR_XIR_UNIT);
                    retirement=XR_XIR_CALL_OOM;
                } else CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.type==XR_XIR_BOOL && result.value.payload==1);
            } else CHECK(attempt && status==XR_XIR_CALL_OOM);
        } else CHECK(attempt && status==XR_XIR_CALL_OOM && !instance);
        if(!attempt)sites=allocation_calls;
        CHECK(published==(attempt?0u:1u));
        if(instance) {
            XirStringRetirement record=xr_string_instance_retire(instance,retirement,config.poll_limit);
            CHECK(record.domain==runtime_domain);xr_xir_domain_drop(record.domain);
        }
        CHECK(live==baseline);
    }
    XrCompileResourceStats retired=allocation_compile_stats(&owner.context);
    CHECK(retired.work==prefix.work && retired.allocated_bytes==prefix.allocated_bytes && retired.live_bytes==prefix.live_bytes);
    fail_at=SIZE_MAX;
    xr_xir_value_drop(&argument);xr_xir_domain_drop(domain);xr_xir_compile_program_drop(program);
    source_fixture_owner_free(&owner);CHECK(!live);return sites;
}

static XrXirOutputStatus allocation_output(void *context, const XrXirOutputGroup *group) {
    CHECK(group && !group->line && group->count == 1);
    XrXirOutputStream stream = group->stream;
    const XrXirValue *value = &group->values[0];
    (void) context; (void) stream;
    return (value->type == XR_XIR_STRING) ? XR_XIR_OUTPUT_OK : XR_XIR_OUTPUT_ERROR;
}
static bool managed_allocation_run(XrXirProgram *program,bool throwing) {
    XrXirDomain *domain=NULL,*runtime_domain=NULL;
    XrXirValue arguments[2]={{0},{0}},owned={0};XrXirInstance *instance=NULL;
    XrXirCallStatus retirement=XR_XIR_CALL_READY;bool completed=false;
    XrXirInstanceConfig config;allocation_instance_config(&config);
    config.output=(XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION,0,allocation_output,NULL};
    XrXirCallStatus admitted=xr_xir_instance_new(program,&config,&instance);
    if(admitted!=XR_XIR_CALL_READY) { CHECK(admitted==XR_XIR_CALL_OOM && !instance);goto done; }
    runtime_domain=xr_string_instance_domain(instance);
    XrXirValueStatus status=xr_xir_domain_new(65536,&domain);
    if(status!=XR_XIR_VALUE_OK) { CHECK(status==XR_XIR_VALUE_OOM);goto done; }
    for(uint32_t i=0;i<2;++i) {
        status=xr_xir_string_new(domain,"x",1,&arguments[i]);
        if(status!=XR_XIR_VALUE_OK) { CHECK(status==XR_XIR_VALUE_OOM);goto done; }
    }
    admitted=xr_xir_instance_start(instance,0,arguments,2);
    if(admitted!=XR_XIR_CALL_READY) { CHECK(admitted==XR_XIR_CALL_OOM);goto done; }
    XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    XrXirCallResult result=polled.outcome;
    if(result.status==XR_XIR_CALL_SUSPENDED) {
        CHECK(xr_xir_instance_resume(instance,polled.epoch,result.wake)==XR_XIR_CALL_READY);
        result=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome;
    }
    if(result.status==(throwing?XR_XIR_CALL_THROWN:XR_XIR_CALL_RETURNED)) {
        CHECK(xr_xir_instance_take_result(instance,&owned)==result.status);
        if(throwing) {
            uint32_t variant=99;CHECK(xr_xir_enum_variant(&owned,&variant)==XR_XIR_VALUE_OK && variant==1);
        }
        completed=true;
    } else { CHECK(result.status==XR_XIR_CALL_OOM);retirement=XR_XIR_CALL_OOM; }
 done:
    if(instance) {
        XirStringRetirement record=xr_string_instance_retire(instance,retirement,config.poll_limit);
        CHECK(record.domain==runtime_domain);xr_xir_domain_drop(record.domain);
    }
    xr_xir_domain_drop(domain);
    xr_xir_value_drop(&arguments[0]);xr_xir_value_drop(&arguments[1]);xr_xir_value_drop(&owned);
    return completed;
}
static bool invoke_allocation_run(XrXirProgram *program,uint32_t mode,uint32_t cancel_at) {
    XrXirInstance *instance=NULL;XrXirValue owned={0};XrXirDomain *runtime_domain=NULL;
    XrXirCallStatus retirement=XR_XIR_CALL_READY;bool completed=false;
    XrXirInstanceConfig config;allocation_instance_config(&config);
    XrXirCallStatus admitted=xr_xir_instance_new(program,&config,&instance);
    if(admitted!=XR_XIR_CALL_READY) { CHECK(admitted==XR_XIR_CALL_OOM && !instance);goto done; }
    runtime_domain=xr_string_instance_domain(instance);
    XrXirValue args[]={{XR_XIR_I64,0,9},{XR_XIR_I64,0,4}};
    admitted=xr_xir_instance_start(instance,0,args,2);
    if(admitted!=XR_XIR_CALL_READY) { CHECK(admitted==XR_XIR_CALL_OOM);goto done; }
    XrXirInstanceResult polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);
    XrXirCallResult result=polled.outcome;uint32_t suspensions=0;
    while(result.status==XR_XIR_CALL_SUSPENDED) {
        CHECK(++suspensions<=2);
        if(suspensions==cancel_at) {
            CHECK(xr_xir_instance_cancel_current(instance)==XR_XIR_CALL_CANCEL_REQUESTED);
            CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_CANCELLED);
        } else CHECK(xr_xir_instance_resume(instance,polled.epoch,result.wake)==XR_XIR_CALL_READY);
        polled=xr_xir_instance_poll_bounded(instance,UINT64_MAX);result=polled.outcome;
    }
    if(result.status==XR_XIR_CALL_OOM) { retirement=XR_XIR_CALL_OOM;goto done; }
    if(cancel_at && suspensions==cancel_at)CHECK(result.status==XR_XIR_CALL_CANCELLED);
    else if(mode==9) {
        CHECK(result.status==XR_XIR_CALL_THROWN);
        CHECK(xr_xir_instance_take_result(instance,&owned)==XR_XIR_CALL_THROWN);
    } else CHECK(result.status==XR_XIR_CALL_RETURNED && result.value.type==XR_XIR_I64 &&
        result.value.payload==(mode==10?44:77));
    completed=true;
 done:
    if(instance) {
        XirStringRetirement record=xr_string_instance_retire(instance,retirement,config.poll_limit);
        CHECK(record.domain==runtime_domain);xr_xir_domain_drop(record.domain);
    }
    xr_xir_value_drop(&owned);return completed;
}
static void invoke_allocation_failures(void) {
    SourceFixtureOwner compiler={0};source_fixture_owner_new(&compiler);
    const uint32_t modes[]={5,9,10,11};
    for(uint32_t m=0;m<4;++m)for(uint32_t cancel=0;cancel<3;++cancel) {
        fail_at=SIZE_MAX;
        XrXirArtifact *artifact=call_fixture(&compiler.context,modes[m]);XrXirProgram *program=NULL;
        CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && program && !artifact);
        size_t baseline=live;XrCompileResourceStats prefix=allocation_compile_stats(&compiler.context);
        allocation_calls=0;CHECK(invoke_allocation_run(program,modes[m],cancel));size_t sites=allocation_calls;
        CHECK(live==baseline && sites);
        for(size_t i=0;i<sites;++i) {
            fail_at=i;allocation_calls=0;
            CHECK(!invoke_allocation_run(program,modes[m],cancel) && live==baseline);
        }
        XrCompileResourceStats retired=allocation_compile_stats(&compiler.context);
        CHECK(retired.work==prefix.work && retired.allocated_bytes==prefix.allocated_bytes &&
            retired.live_bytes==prefix.live_bytes);
        fail_at=SIZE_MAX;xr_xir_compile_program_drop(program);CHECK(live==1);
        printf("Invoke physical release: mode=%u cancel=%u allocation failure sites=%zu\n",modes[m],cancel,sites);
    }
    source_fixture_owner_free(&compiler);
}
static size_t managed_allocation_failures(void) {
    size_t total=0;
    for(uint32_t mode=0;mode<2;++mode) {
        fail_at=SIZE_MAX;
        SourceFixtureOwner owner={0};source_fixture_owner_new(&owner);
        XrXirArtifact *artifact=string_fixture(&owner.context,mode);XrXirProgram *program=NULL;
        CHECK(xr_xir_compile_vm_program_take(&artifact,&program)==XR_XIR_OK && program && !artifact);
        size_t baseline=live;XrCompileResourceStats prefix=allocation_compile_stats(&owner.context);
        allocation_calls=0;CHECK(managed_allocation_run(program,mode!=0));size_t sites=allocation_calls;
        CHECK(live==baseline && sites);
        for(size_t i=0;i<sites;++i) {
            fail_at=i;allocation_calls=0;
            CHECK(!managed_allocation_run(program,mode!=0) && live==baseline);
        }
        XrCompileResourceStats retired=allocation_compile_stats(&owner.context);
        CHECK(retired.work==prefix.work && retired.allocated_bytes==prefix.allocated_bytes &&
            retired.live_bytes==prefix.live_bytes);
        fail_at=SIZE_MAX;xr_xir_compile_program_drop(program);
        source_fixture_owner_free(&owner);CHECK(!live);total+=sites;
        printf("Managed call release: throw=%u allocation failure sites=%zu\n",mode,sites);
    }
    return total;
}

typedef struct AllocationFrame { bool entered; XrXirValue argument; } AllocationFrame;
static XrXirAction allocation_resume(XrXirCallView *view) {
    AllocationFrame *frame = view->state;
    if (!frame->entered && view->arguments[0].payload) {
        frame->entered = true;
        frame->argument = (XrXirValue) {XR_XIR_I64, 0, view->arguments[0].payload - 1};
        return (XrXirAction) {XR_XIR_ACTION_CALL, 0, &frame->argument, 1, {0, 0, 0}, {0}, 0};
    }
    return (XrXirAction) {XR_XIR_ACTION_RETURN, 0, NULL, 0, {XR_XIR_I64, 0,
        frame->entered ? view->inbox.value.payload + 1 : 0}, {0}, 0};
}

static size_t call_allocation_failures(void) {
    XrXirType type = XR_XIR_I64;
    XrXirCallEntry entry = {XR_XIR_CALL_ABI_VERSION, &type, 1, XR_XIR_I64, sizeof(AllocationFrame), allocation_resume, NULL, NULL, 0, 0};
    size_t expected_calls = 0;
    for (size_t attempt = 0; attempt <= expected_calls; ++attempt) {
        fail_at = attempt ? attempt - 1 : SIZE_MAX;
        allocation_calls = 0;
        XrXirCallAccounting accounting = {0};
        XrXirCallConfig config; CHECK(xr_xir_call_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY); config.entries = &entry; config.entry_count = 1; config.instance = NULL; config.byte_limit = 65536; config.poll_limit = 100; config.depth_limit = 10; config.accounting = &accounting; config.output = (XrXirOutputProvider) {0}; config.admission = (XrXirValueAdmission) {0};
        XrXirValue argument = {XR_XIR_I64, 0, 3};
        XrXirCall *call = NULL;
        XrXirCallStatus status = xr_xir_call_new(&config, 0, &argument, 1, &call);
        if (status == XR_XIR_CALL_READY) {
            XrXirCallResult result = xr_xir_call_poll_bounded(call, UINT64_MAX);
            CHECK(result.status == (attempt ? XR_XIR_CALL_OOM : XR_XIR_CALL_RETURNED));
            if (!attempt) CHECK(result.value.payload == 3);
            CHECK(xr_xir_call_free(call) == XR_XIR_CALL_READY);
        } else CHECK(attempt && status == XR_XIR_CALL_OOM && !call);
        if (!attempt) expected_calls = allocation_calls;
        CHECK(live == 0 && accounting.live_bytes == 0 && accounting.allocations == accounting.frees);
    }
    fail_at = SIZE_MAX;
    return expected_calls;
}

#include "xir_program_fixture.h"
static XrXirOutputStatus program_allocation_output(void *context, const XrXirOutputGroup *group) {
    CHECK(group && !group->line && group->count == 1);
    XrXirOutputStream stream = group->stream;
    const XrXirValue *value = &group->values[0];
    (void) context; (void) stream; (void) value; return XR_XIR_OUTPUT_OK;
}
static size_t program_allocation_failures(void) {
    size_t expected_calls = 0;
    for (size_t attempt = 0; attempt <= expected_calls; ++attempt) {
        fail_at = SIZE_MAX;
        AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
        XrXirArtifact *artifact=program_fixture(&owner.context,0),*original=artifact;
        XrXirProgram *program = NULL;
        allocation_calls = 0; fail_at = attempt ? attempt - 1 : SIZE_MAX;
        XrXirStatus sealed = xr_xir_compile_vm_program_take(&artifact, &program);
        if (sealed != XR_XIR_OK) {
            CHECK(attempt && sealed == XR_XIR_OUT_OF_MEMORY && !program && artifact == original);
        } else {
            CHECK(!artifact);
            XrXirInstanceConfig config; CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
            config.output = (XrXirOutputProvider) {XR_XIR_CALL_ABI_VERSION, 0, program_allocation_output, NULL};
            XrXirInstance *instance = NULL;
            XrXirCallStatus status = xr_xir_instance_new(program, &config, &instance);
            if (status != XR_XIR_CALL_READY) CHECK(attempt && status == XR_XIR_CALL_OOM && !instance);
            else {
                XrXirDomain *runtime_domain=xr_string_instance_domain(instance);
                status = xr_xir_instance_start(instance, 3, NULL, 0);
                bool executed=status==XR_XIR_CALL_READY;
                if (executed) status = xr_xir_instance_poll_bounded(instance, UINT64_MAX).outcome.status;
                CHECK(status == (attempt ? XR_XIR_CALL_OOM : XR_XIR_CALL_RETURNED));
                if (xr_xir_instance_state(instance) == XR_XIR_INSTANCE_FAILED)
                    CHECK(xr_xir_instance_start(instance, 3, NULL, 0) == XR_XIR_CALL_OOM);
                /* Uncommitted start rejection leaves no execution failure.
                 * A driven OOM is the executor's sticky retirement result. */
                XrXirCallStatus retirement=executed && status==XR_XIR_CALL_OOM?XR_XIR_CALL_OOM:XR_XIR_CALL_READY;
                XirStringRetirement record=xr_string_instance_retire(instance,retirement,config.poll_limit);
                CHECK(record.domain==runtime_domain);xr_xir_domain_drop(record.domain);
            }
            xr_xir_compile_program_drop(program);
        }
        if (!attempt) expected_calls = allocation_calls;
        xr_xir_compile_artifact_free(artifact);
        fail_at=SIZE_MAX;allocation_compile_owner_drop(&owner);CHECK(live==0);
    }
    fail_at = SIZE_MAX;
    return expected_calls;
}
static size_t declaration_allocation_failures(void) {
    AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
    XrXirArtifact *checked=program_fixture_checked(&owner.context,0),*closed=NULL,*fixture=NULL;
    CHECK(xr_xir_compile_specialize(checked,&closed,NULL)==XR_XIR_OK);
    CHECK(xr_xir_compile_lower(closed,&fixture_target,&fixture,NULL)==XR_XIR_OK);
    size_t baseline=live,total=0;
    for(uint32_t phase=0;phase<3;++phase) {
        size_t sites=0;
        for(size_t attempt=0;attempt<=sites;++attempt) {
            allocation_calls=0;fail_at=attempt?attempt-1:SIZE_MAX;
            XrXirArtifact *output=NULL;XrXirCSource source={0};
            XrXirModule module=*xr_xir_compile_artifact_module(checked);module.stage=XR_XIR_BUILT;
            XrXirStatus status=phase==0?xir_fixture_check(&owner.context, &module, &output, NULL):
                phase==1?xr_xir_compile_lower(closed,&fixture_target,&output,NULL):
                xr_xir_compile_emit_c(fixture,"owned_program",200000,&source);
            CHECK(status==(attempt?XR_XIR_OUT_OF_MEMORY:XR_XIR_OK));
            if(!attempt)sites=allocation_calls;else CHECK(!output && !source.text);
            xr_xir_compile_artifact_free(output);xr_xir_compile_c_source_free(&source);CHECK(live==baseline);
        }
        fail_at=SIZE_MAX;total+=sites;
    }
    xr_xir_compile_artifact_free(fixture);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    allocation_compile_owner_drop(&owner);CHECK(!live);return total;
}

#include "xir_nominal_fixture.h"
static XrXirStatus nominal_copy_operation(const XrXirCompileContext *context,void *opaque) {
    NominalFixture *f=opaque;XrXirNominalTable *copy=NULL;
    XrXirStatus status=xr_xir_compile_nominal_clone(context,&f->table,NULL,&copy);
    if(status==XR_XIR_OK)CHECK(copy && copy->count==2 && copy->declarations[0].fields!=f->fields);
    else CHECK(!copy);
    xr_xir_compile_nominal_free(copy);return status;
}
static void nominal_allocation_failures(void) {
    NominalFixture f;nominal_fixture(&f);
    allocation_compile_operation_cases("nominal clone",nominal_copy_operation,&f);
    AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
    XrXirNominalTable *copy=NULL;f.fields[0].type=XR_XIR_UNIT;allocation_calls=0;
    CHECK(xr_xir_compile_nominal_clone(&owner.context,&f.table,NULL,&copy)==XR_XIR_BAD_TYPE && !copy);
    CHECK(!allocation_calls);allocation_compile_owner_drop(&owner);
}
static XrXirStatus nominal_pool_copy_operation(const XrXirCompileContext *context,void *opaque) {
    XrXirTypes *types=opaque,*copy=NULL;
    XrXirStatus status=xr_xir_compile_types_clone(context,types,&copy);
    if(status==XR_XIR_OK)CHECK(copy && copy->nominals && copy->nodes!=types->nodes);
    else CHECK(!copy);
    xr_xir_compile_types_free(copy);return status;
}
static void nominal_pool_allocation_failures(void) {
    NominalFixture f;nominal_fixture(&f);
    XrXirType argument=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode nodes[]={{XR_XIR_TYPE_ARRAY,XR_XIR_STRING,NULL,0,XR_XIR_UNIT,0,0,{0}},
        {XR_XIR_TYPE_NOMINAL,XR_XIR_UNIT,NULL,0,XR_XIR_UNIT,0,0,{0,&argument,1,NULL,0}}};
    XrXirTypes types={nodes,2,&f.table,NULL};
    AllocationCompileOwner owner={0};allocation_compile_owner_new(&owner,&allocation_compile_limits);
    CHECK(xr_xir_compile_types_structure_verify(&owner.context,&types)==XR_XIR_OK);
    allocation_compile_owner_drop(&owner);
    allocation_compile_operation_cases("nominal type pool clone",nominal_pool_copy_operation,&types);
}
static XrXirStatus allocation_check_operation(const XrXirCompileContext *context,void *opaque) {
    XrXirArtifact *artifact=NULL;
    XrXirStatus status=xir_fixture_check(context, opaque, &artifact, NULL);
    if(status!=XR_XIR_OK)CHECK(!artifact);
    xr_xir_compile_artifact_free(artifact);return status;
}

#include "xir_class_owned_allocations.h"
#include "xir_default_invoke_allocations.h"
#include "xir_allocation_phase_cases.h"
static void allocation_remaining_resources(void) {
    allocation_case_name="resources";allocation_stage="complete-resource-cases";
    allocation_case_scalar=0;allocation_case_mode=0;
    default_invoke_allocation_cases();
    class_owned_allocations();
    type_temporary_budget_cases();
    type_temporary_allocation_failures();
    constraint_temporary_cases();
    access_temporary_cases();
    generic_method_clone_allocations();
    generic_method_packet_allocations();
    implementation_copy_allocations();
    implementation_packet_allocations();
    constraint_packet_allocation_failures();
    interface_packet_allocation_failures();
    nominal_pool_allocation_failures();
    nominal_allocation_failures();
    segment_cases();
    XrXirInstruction ops[] = {
        {XR_XIR_CONST_INT, XR_XIR_I64, {0, 0}, {0, 0}, 42, {0}},
        {XR_XIR_COPY, XR_XIR_I64, {0, 0}, {0, 0}, 0, {0}},
        {XR_XIR_RETURN, XR_XIR_UNIT, {1, 0}, {0, 0}, 0, {0}},
    };
    XrXirBlock block = {0, 3, 0, 0};
    XrXirType parameter = XR_XIR_I64;
    XrXirFunction functions[] = {
        {"first", 5, NULL, 0, XR_XIR_I64, &block, 1, ops, 3, NULL, 0},
        {"second", 6, &parameter, 1, XR_XIR_I64, &block, 1, ops, 3, NULL, 0},
    };
    XrXirModule module = {XR_XIR_BUILT, functions, 2, NULL, NULL, NULL, NULL, XR_XIR_PROGRAM, NULL};
    XrXirArtifact *checked = NULL, *lowered = NULL;
    allocation_compile_operation_cases("two-function Check",allocation_check_operation,&module);
    AllocationCompileOwner compiler={0};allocation_compile_owner_new(&compiler,&allocation_compile_limits);
    size_t compiler_live=live;
    allocation_calls = 0;
    CHECK(xir_fixture_check(&compiler.context, &module, &checked, NULL) == XR_XIR_OK);
    size_t check_calls = allocation_calls;
    CHECK(live > 0);
    xr_xir_compile_artifact_free(checked);
    CHECK(live == compiler_live);
    for (size_t i = 0; i < check_calls; ++i) {
        allocation_calls = 0;
        fail_at = i;
        checked = NULL;
        CHECK(xir_fixture_check(&compiler.context, &module, &checked, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(checked == NULL);
        CHECK(live == compiler_live);
    }
    fail_at = SIZE_MAX;
    CHECK(xir_fixture_check(&compiler.context, &module, &checked, NULL) == XR_XIR_OK);
    size_t checked_live = live;
    allocation_calls = 0;
    CHECK(xr_xir_compile_lower(checked, &fixture_target, &lowered, NULL) == XR_XIR_OK);
    size_t lower_calls = allocation_calls;
    xr_xir_compile_artifact_free(lowered);
    CHECK(live == checked_live);
    for (size_t i = 0; i < lower_calls; ++i) {
        allocation_calls = 0;
        fail_at = i;
        lowered = NULL;
        CHECK(xr_xir_compile_lower(checked, &fixture_target, &lowered, NULL) == XR_XIR_OUT_OF_MEMORY);
        CHECK(lowered == NULL);
        CHECK(live == checked_live);
    }
    fail_at = SIZE_MAX;
    CHECK(xr_xir_compile_verify_v2(&compiler.context, xr_xir_compile_artifact_module(checked), xr_xir_compile_artifact_construction(checked), NULL) == XR_XIR_OK);
    CHECK(xr_xir_compile_lower(checked, &fixture_target, &lowered, NULL) == XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);
    size_t lowered_live = live;
    allocation_calls = 0;
    XrXirRunContext context = {3, 16, 0, 0, 0, 0};
    XrXirValue result;
    CHECK(xr_xir_compile_vm_run(lowered, 0, &context, NULL, 0, &result) == XR_XIR_RUN_OK);
    CHECK(result.payload == 42 && live == lowered_live);
    size_t run_calls = allocation_calls;
    for (size_t i = 0; i < run_calls; ++i) {
        allocation_calls = 0;
        fail_at = i;
        context = (XrXirRunContext) {3, 16, 0, 0, 0, 0};
        result = (XrXirValue) {99, 99, 99};
        CHECK(xr_xir_compile_vm_run(lowered, 0, &context, NULL, 0, &result) == XR_XIR_RUN_OUT_OF_MEMORY);
        CHECK(result.type == 0 && result.reserved == 0 && result.payload == 0);
        CHECK(context.live_bytes == 0 && context.allocations == context.frees);
        CHECK(live == lowered_live);
    }
    fail_at = SIZE_MAX;
    allocation_calls = 0;
    XrXirCSource source={0};
    CHECK(xr_xir_compile_emit_leaf_c(lowered, "allocation", 65536, &source) == XR_XIR_OK);
    size_t emit_calls = allocation_calls;
    xr_xir_compile_c_source_free(&source);
    CHECK(live == lowered_live);
    for (size_t i = 0; i < emit_calls; ++i) {
        allocation_calls = 0;
        fail_at = i;
        CHECK(xr_xir_compile_emit_leaf_c(lowered, "allocation", 65536, &source) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!source.text && !source.length && live == lowered_live);
    }
    fail_at = SIZE_MAX;
    allocation_calls = 0;
    CHECK(xr_xir_compile_emit_c(lowered, "resumable", 65536, &source) == XR_XIR_OK);
    size_t resume_emit_calls = allocation_calls;
    xr_xir_compile_c_source_free(&source);
    CHECK(live == lowered_live);
    for (size_t i = 0; i < resume_emit_calls; ++i) {
        allocation_calls = 0;
        fail_at = i;
        CHECK(xr_xir_compile_emit_c(lowered, "resumable", 65536, &source) == XR_XIR_OUT_OF_MEMORY);
        CHECK(!source.text && !source.length && live == lowered_live);
    }
    fail_at = SIZE_MAX;
    xr_xir_compile_artifact_free(lowered);
    CHECK(live == compiler_live);
    allocation_compile_owner_drop(&compiler);CHECK(!live);
    size_t call_sites = call_allocation_failures();
    printf("XIR physical release passed at %zu check, %zu lower, %zu VM, %zu emit allocation sites\n",
           check_calls, lower_calls, run_calls, emit_calls);
    printf("Resumable physical release passed at %zu emitter and %zu activation/frame allocation sites\n",
           resume_emit_calls, call_sites);
    printf("Managed VM admission and execution physical release: %zu allocation sites\n", managed_allocation_failures());
    printf("Declaration stage/emission physical release: %zu allocation sites\n", declaration_allocation_failures());
    printf("Program seal, instance and initialization physical release: %zu allocation sites\n", program_allocation_failures());
    printf("Write-result activation and renderer physical release: %zu allocation sites\n", write_allocation_failures());
    invoke_allocation_failures();
    phi_snapshot_failure();
    instance_admission_cases();
    initialization_region_allocations();
}

static void allocation_phase_matrix(bool axes) {
    function_allocation_phase_case(false);function_allocation_phase_case(true);
    function_producer_allocation_phase_case();
    for(unsigned mode=0;mode<8;++mode)invocation_runtime_allocation_phase_case(mode);
    if(axes) {
        allocation_phase_case_begin("runtime",0,0);allocation_stage="resource-axes";
        invocation_runtime_owner_axes();CHECK(!live);
    }
    for(unsigned mode=0;mode<8;++mode)invocation_direct_allocation_phase_case(mode);
    if(axes) {
        allocation_phase_case_begin("direct",0,0);allocation_stage="resource-axes";
        invocation_direct_owner_axes();CHECK(!live);
    }
    for(unsigned scalar=0;scalar<2;++scalar) {
        for(unsigned mode=0;mode<8;++mode)invocation_capture_allocation_phase_case(mode,scalar!=0);
        if(axes) {
            allocation_phase_case_begin("capture",scalar,0);allocation_stage="resource-axes";
            invocation_capture_owner_axes(scalar!=0);CHECK(!live);
        }
        for(unsigned mode=0;mode<8;++mode)invocation_return_capture_allocation_phase_case(mode,scalar!=0);
        if(axes) {
            allocation_phase_case_begin("return_capture",scalar,0);allocation_stage="resource-axes";
            invocation_return_capture_owner_axes(scalar!=0);CHECK(!live);
        }
        for(unsigned mode=0;mode<8;++mode)invocation_function_cell_allocation_phase_case(mode,scalar!=0);
        if(axes) {
            allocation_phase_case_begin("function_cell",scalar,0);allocation_stage="resource-axes";
            invocation_function_cell_owner_axes(scalar!=0);CHECK(!live);
        }
    }
    CHECK(allocation_phase_cases==67);
    printf("FI map total: cases=%zu original_ordinals=%zu compiler=%zu runtime=%zu bridge=%zu axes=%u\n",
        allocation_phase_cases,allocation_phase_ordinals,allocation_phase_compiler_ordinals,
        allocation_phase_runtime_ordinals,allocation_phase_bridge_ordinals,axes?88u:0u);
}
#include "xir_allocation_case_workers.h"
int main(int argc,char **argv) {
    if(allocation_worker_dispatch(argc,argv))return 0;
    CHECK(argc<=2);
    if(argc==2) {
        if(!strcmp(argv[1],"--bookkeeping")) {allocation_bookkeeping_cases();return 0;}
        else if(!strcmp(argv[1],"--semantics"))allocation_phase_selection=0;
        else if(!strcmp(argv[1],"--producer"))allocation_phase_selection=ALLOCATION_PHASE_COMPILER;
        else if(!strcmp(argv[1],"--runtime"))allocation_phase_selection=ALLOCATION_PHASE_RUNTIME;
        else if(!strcmp(argv[1],"--first-case")) {
            function_allocation_phase_case(false);CHECK(allocation_phase_cases==1);return 0;
        } else if(!strcmp(argv[1],"--resources")) { allocation_remaining_resources();return 0; }
        else {fprintf(stderr,"unknown allocation test selection: %s\n",argv[1]);return 2;}
    }
    if(argc==1)allocation_remaining_resources();
    allocation_phase_matrix(true);
    CHECK(!live && !class_live_bytes);return 0;
}
