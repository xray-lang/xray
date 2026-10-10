/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_function_producer_owner_cases.h - True bodies and producer lifetime
 *
 * KEY CONCEPT:
 *   Full construction and lowering precede Program admission. Native callbacks
 *   implement the literal bodies; metadata-only synthetic proofs are not used.
 */
#ifndef XIR_FUNCTION_PRODUCER_OWNER_CASES_H
#define XIR_FUNCTION_PRODUCER_OWNER_CASES_H
#include "xir/xxir_program_internal.h"
#include "xir/xxir_instance_function_internal.h"
#include "xir/xxir_value_internal.h"
#include "xir/xxir_type_arena.h"
typedef struct ProducerRuntimeEnvironment { uint32_t source; } ProducerRuntimeEnvironment;
typedef struct ProducerRuntimeFrame { uint32_t phase;XrXirValue owned; } ProducerRuntimeFrame;
typedef struct ProducerRuntimeWitness {
    unsigned releases;
    ProducerRuntimeEnvironment environments[16];
} ProducerRuntimeWitness;
typedef struct ProducerRuntimeFixture {
    XrXirTypeNode nodes[2];XrXirTypes types;
    XrXirInstruction init[1],entry[2],make[3],body[1],forward[2],nested[2];
    uint32_t capture,forward_capture;XrXirType i64,callback;
    XrXirBlock blocks[6];XrXirFunction functions[6];
    XrXirFunctionIdentity identities[6];XrXirSourceModule source;
    XrXirDeclarations declarations;XrXirModule module;
} ProducerRuntimeFixture;
static void producer_runtime_fixture(ProducerRuntimeFixture *f) {
    memset(f,0,sizeof(*f));f->i64=XR_XIR_I64;f->callback=(XrXirType)256;
    f->nodes[0]=(XrXirTypeNode){.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,
        .flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED};
    f->nodes[1]=f->nodes[0];f->nodes[1].flags=XR_XIR_CALLABLE_ROOT_NONE;
    f->types=(XrXirTypes){f->nodes,2,NULL,NULL};
    f->init[0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->entry[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=17};
    f->entry[1]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->make[0]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    f->make[1]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=f->callback,.args={0,1},.immediate=3};
    f->make[2]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->body[0]=(XrXirInstruction){.op=XR_XIR_RETURN};
    f->forward[0]=(XrXirInstruction){.op=XR_XIR_CALL_INDIRECT,.type=XR_XIR_I64};
    f->forward[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    f->nested[0]=(XrXirInstruction){.op=XR_XIR_FUNCTION_REF,.type=f->callback,.args={0,1},.immediate=4};
    f->nested[1]=(XrXirInstruction){.op=XR_XIR_RETURN,.args={1,0}};
    uint32_t counts[6]={1,2,3,1,2,2};
    XrXirInstruction *instructions[6]={f->init,f->entry,f->make,f->body,f->forward,f->nested};
    const char *names[6]={"init","entry","make","body","forward","nested"};
    for (uint32_t n=0;n<6;++n) {
        f->blocks[n]=(XrXirBlock){.count=counts[n]};
        f->functions[n]=(XrXirFunction){.name=names[n],.name_length=(uint32_t)strlen(names[n]),
            .result=n?((n==2||n==5)?f->callback:XR_XIR_I64):XR_XIR_UNIT,
            .blocks=&f->blocks[n],.block_count=1,.instructions=instructions[n],.instruction_count=counts[n]};
        f->identities[n]=(XrXirFunctionIdentity){.exported=(n!=0&&n!=3&&n!=4)};
    }
    f->functions[2].operands=&f->capture;f->functions[2].operand_count=1;
    f->functions[3].parameters=&f->i64;f->functions[3].parameter_count=1;
    f->functions[4].parameters=&f->callback;f->functions[4].parameter_count=1;
    f->functions[5].parameters=&f->callback;f->functions[5].parameter_count=1;
    f->functions[5].operands=&f->forward_capture;f->functions[5].operand_count=1;
    f->source=(XrXirSourceModule){"producer",8,NULL,0,0};
    f->declarations=(XrXirDeclarations){.modules=&f->source,.module_count=1,.functions=f->identities,
        .entry_function=1};
    f->module=(XrXirModule){.stage=XR_XIR_BUILT,.functions=f->functions,.function_count=6,
        .types=&f->types,.declarations=&f->declarations,.linkage_kind=XR_XIR_PROGRAM};
}
static XrXirAction producer_runtime_fault(XrXirCallStatus status) {
    return (XrXirAction){XR_XIR_ACTION_FAULT,0,NULL,0,{XR_XIR_I64,0,status},{0},0};
}
static void producer_runtime_frame_release(XrXirCallView *view,XrXirCallStatus status) {
    (void)status;xr_xir_value_drop(&((ProducerRuntimeFrame *)view->state)->owned);
}
static void producer_runtime_code_release(void *owner) { ++((ProducerRuntimeWitness *)owner)->releases; }
static XrXirCallStatus producer_runtime_negative(XrXirCallView *view,const XrXirValue *actual) {
    const XrXirFunctionBinding *binding=xr_xir_function_binding(actual);
    CHECK(binding);XirEffectProducerView real={0};
    XrXirCallStatus status=xr_xir_instance_function_producer(view,actual,&real);
    if (status!=XR_XIR_CALL_READY) return status;
    CHECK(real.capture_count==binding->capture_count && real.target==binding->entry);
    XirFunctionProducer *origin=(XirFunctionProducer *)xr_xir_function_producer(actual);
    CHECK(origin);XirFunctionProducer saved=*origin;XirEffectProducerView untouched=real;
    origin->owner=(const void *)(uintptr_t)1;
    CHECK(xr_xir_instance_function_producer(view,actual,&untouched)==XR_XIR_CALL_BAD_ARGUMENT &&
        !memcmp(&real,&untouched,sizeof(real)));*origin=saved;
    origin->site=UINT32_MAX;
    CHECK(xr_xir_instance_function_producer(view,actual,&untouched)==XR_XIR_CALL_BAD_ARGUMENT &&
        !memcmp(&real,&untouched,sizeof(real)));*origin=saved;
    origin->instruction=UINT32_MAX;
    CHECK(xr_xir_instance_function_producer(view,actual,&untouched)==XR_XIR_CALL_BAD_ARGUMENT &&
        !memcmp(&real,&untouched,sizeof(real)));*origin=saved;
    XrXirValue public_value={0},weakened={0};
    status=xr_xir_instance_function(view,(XrXirType)actual->type,binding->entry,
        binding->captures,binding->capture_count,&public_value);
    if (status==XR_XIR_CALL_READY) {
        untouched=real;
        CHECK(!xr_xir_function_producer(&public_value));
        CHECK(xr_xir_instance_function_producer(view,&public_value,&untouched)==XR_XIR_CALL_BAD_ARGUMENT &&
            !memcmp(&real,&untouched,sizeof(real)));
        status=xr_xir_instance_weaken_function(view,(XrXirType)actual->type,actual,&weakened);
        if (status==XR_XIR_CALL_READY) {
            CHECK(!xr_xir_function_producer(&weakened));
            CHECK(xr_xir_instance_function_producer(view,&weakened,&untouched)==XR_XIR_CALL_BAD_ARGUMENT &&
                !memcmp(&real,&untouched,sizeof(real)));
        }
    }
    xr_xir_value_drop(&public_value);xr_xir_value_drop(&weakened);return status;
}
static XrXirAction producer_runtime_resume(XrXirCallView *view) {
    ProducerRuntimeFrame *frame=view->state;
    uint32_t source=((ProducerRuntimeEnvironment *)view->environment)->source;
    XrXirCallStatus status=XR_XIR_CALL_READY;
    if (!source) return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,{0},{0},0};
    if (source==1) return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,{XR_XIR_I64,0,17},{0},0};
    if (source==3) return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,view->arguments[0],{0},0};
    if (source==4) {
        if (!frame->phase++) {
            uint32_t target=UINT32_MAX;
            status=xr_xir_instance_resolve_function(view,&view->arguments[0],&target);
            if (status==XR_XIR_CALL_READY)
                return (XrXirAction){XR_XIR_ACTION_CALL,target,NULL,0,view->arguments[0],{0},0};
        } else return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,view->inbox.value,{0},0};
    } else if (source==2) {
        XrXirValue capture={XR_XIR_I64,0,42};
        XrXirValue rejected={0};
        CHECK(xr_xir_instance_function_at(view,0,&capture,1,&rejected)==XR_XIR_CALL_BAD_ARGUMENT && !rejected.payload);
        CHECK(xr_xir_instance_function_at(view,1,&capture,0,&rejected)==XR_XIR_CALL_BAD_ARGUMENT && !rejected.payload);
        status=xr_xir_instance_function_at(view,1,&capture,1,&frame->owned);
    } else if (source==5) {
        status=producer_runtime_negative(view,&view->arguments[0]);
        if (status==XR_XIR_CALL_READY)
            status=xr_xir_instance_function_at(view,0,&view->arguments[0],1,&frame->owned);
    }
    if (status!=XR_XIR_CALL_READY) return producer_runtime_fault(status);
    XirEffectProducerView actual={0};
    status=xr_xir_instance_function_producer(view,&frame->owned,&actual);
    if (status!=XR_XIR_CALL_READY) return producer_runtime_fault(status);
    CHECK(actual.capture_count==1 && actual.instruction==(source==2?1u:0u));
    return (XrXirAction){XR_XIR_ACTION_RETURN,0,NULL,0,frame->owned,{0},0};
}
static XrXirStatus producer_runtime_seal(ProducerRuntimeWitness *witness,XrXirProgram **output,
    uint32_t *make,uint32_t *nested) {
    NativeFixtureOwner owner={0};ProducerRuntimeFixture fixture;producer_runtime_fixture(&fixture);
    XrXirStatus status=native_fixture_owner_new(&owner);
    XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    if (status==XR_XIR_OK) status=xir_fixture_check(&owner.context,&fixture.module,&checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(checked,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_specialize(checked,&closed,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(closed,NULL);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    if (status==XR_XIR_OK) status=xr_xir_compile_lower(closed,&target,&lowered,NULL);
    if (status==XR_XIR_OK) status=xr_xir_compile_artifact_verify(lowered,NULL);
    if (status==XR_XIR_OK) {
        const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
        CHECK(module->function_count<=16 && module->provenance && module->provenance->origins);
        XrXirCallEntry entries[16]={0};*make=UINT32_MAX;*nested=UINT32_MAX;
        for (uint32_t f=0;f<module->function_count;++f) {
            uint32_t source=module->provenance->origins[f].function;CHECK(source<6);
            witness->environments[f].source=source;
            if (source==2) *make=f;
            if (source==5) *nested=f;
            entries[f]=(XrXirCallEntry){XR_XIR_CALL_ABI_VERSION,module->functions[f].parameters,
                module->functions[f].parameter_count,module->functions[f].result,sizeof(ProducerRuntimeFrame),
                producer_runtime_resume,producer_runtime_frame_release,&witness->environments[f],0,0};
        }
        CHECK(*make!=UINT32_MAX && *nested!=UINT32_MAX);
        XrXirProgramSpec spec={XR_XIR_PROGRAM_ABI_VERSION,target,entries,module->function_count,
            module->declarations,{witness,producer_runtime_code_release},module->types,
            xr_xir_compile_program_proof(lowered)};
        status=xr_xir_compile_program_seal(&owner.context,&spec,output);
    }
    xr_xir_compile_artifact_free(lowered);xr_xir_compile_artifact_free(closed);
    xr_xir_compile_artifact_free(checked);memset(&fixture,0,sizeof(fixture));native_fixture_owner_free(&owner);
    return status;
}
static XrXirCallStatus producer_runtime_take(XrXirInstance *instance,XrXirValue *output) {
    XrXirCallStatus status=xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status;
    if (status==XR_XIR_CALL_RETURNED) {
        CHECK(xr_xir_instance_take_result(instance,output)==XR_XIR_CALL_RETURNED);return XR_XIR_CALL_READY;
    }
    return status;
}
static bool function_producer_owner_run(void) {
    ProducerRuntimeWitness witness={0};XrXirProgram *program=NULL;
    uint32_t make=0,nested=0;XrXirStatus sealed=producer_runtime_seal(&witness,&program,&make,&nested);
    if (sealed!=XR_XIR_OK) { CHECK(sealed==XR_XIR_OUT_OF_MEMORY && !program && !witness.releases);return false; }
    CHECK(program->permissions && program->permissions->effects && program->permissions->reference_count>=2);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL,*other=NULL;XrXirValue first={0},second={0},closure={0},copy={0},result={0};
    XrXirCallStatus status=xr_xir_instance_new(program,&config,&instance);bool success=false;
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_new(program,&config,&other);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,make,NULL,0);
    if (status==XR_XIR_CALL_READY) status=producer_runtime_take(instance,&first);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start(instance,make,NULL,0);
    if (status==XR_XIR_CALL_READY) status=producer_runtime_take(instance,&second);
    if (status==XR_XIR_CALL_READY) {
        const XirFunctionProducer *a=xr_xir_function_producer(&first),*b=xr_xir_function_producer(&second);
        CHECK(a && b && a->owner==program->permissions->effects && a->site==b->site &&
            a->function==b->function && a->instruction==b->instruction && first.payload!=second.payload);
        CHECK(xr_xir_instance_start_function(other,&first,NULL,0)==XR_XIR_CALL_BAD_ARGUMENT);
        status=xr_xir_instance_start(instance,nested,&first,1);
    }
    if (status==XR_XIR_CALL_READY) status=producer_runtime_take(instance,&closure);
    if (status==XR_XIR_CALL_READY) status=xr_xir_instance_start_function(instance,&closure,NULL,0);
    if (status==XR_XIR_CALL_READY) status=producer_runtime_take(instance,&result);
    if (status==XR_XIR_CALL_READY) { CHECK(result.type==XR_XIR_I64 && result.payload==42);success=true; }
    else CHECK(status==XR_XIR_CALL_OOM || status==XR_XIR_CALL_LIMIT);
    CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    CHECK(xr_xir_instance_free(other)==XR_XIR_CALL_READY);xr_xir_compile_program_drop(program);
    if (closure.type) {
        CHECK(!witness.releases && xr_xir_value_copy(&closure,&copy)==XR_XIR_VALUE_OK);
        const XirFunctionProducer *a=xr_xir_function_producer(&closure),*b=xr_xir_function_producer(&copy);
        CHECK(a && b && a==b && a->owner);
    }
    xr_xir_value_drop(&first);xr_xir_value_drop(&second);xr_xir_value_drop(&closure);xr_xir_value_drop(&result);
    if (copy.type) CHECK(!witness.releases);
    xr_xir_value_drop(&copy);CHECK(witness.releases==1);return success;
}
#endif // XIR_FUNCTION_PRODUCER_OWNER_CASES_H
