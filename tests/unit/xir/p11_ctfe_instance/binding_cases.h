/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * binding_cases.h - Actual Source definitions and canonical closed call bindings
 *
 * KEY CONCEPT:
 *   Genuine generic definitions survive producer death and independent packet receiving.
 */
#include "xir/xxir_compile_memory.h"
static XrXirStatus binding_source(XrXirCompileContext *context,XrXirArtifact **output,unsigned mode) {
    XirSourceComptimeInstances stage={0};
    XrXirStatus status=comptime_instances_operation(context,comptime_instances_text("scalar"),&stage,
        comptime_instances_limits(),XR_XIR_CTFE_MAX_CALL_DEPTH,false);
    if (status!=XR_XIR_OK) return status;
    XirCtfeInstanceView views[2]={0};XirSourceComptimeBinding actual[2]={0};
    XrXirCtfeStatus queried=xr_xir_compile_source_comptime_instances_view(&stage,views,actual,2);
    if (queried!=XR_XIR_CTFE_OK) {
        xr_xir_compile_source_comptime_instances_free(&stage);
        return queried==XR_XIR_CTFE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:queried==XR_XIR_CTFE_BUDGET?XR_XIR_BUDGET:XR_XIR_BAD_STRUCTURE;
    }
    CHECK(views[0].definition==views[1].definition && views[0].value->payload==42 && views[1].value->payload==42);
    const XrXirArtifact *original=views[0].definition;uint32_t count=original->module.function_count;
    XrXirFunction *functions=xir_compile_calloc(context,count+1,sizeof(*functions),&status);
    XrXirFunctionIdentity *identities=status==XR_XIR_OK?xir_compile_calloc(context,count+1,sizeof(*identities),&status):NULL;
    XrXirGeneric *generics=status==XR_XIR_OK?xir_compile_calloc(context,count+1,sizeof(*generics),&status):NULL;
    if (status!=XR_XIR_OK) goto done;
    if (!xir_compile_work(context,(uint64_t)count*(sizeof(*functions)+sizeof(*identities)+sizeof(*generics)))) {
        status=XR_XIR_BUDGET;goto done;
    }
    memcpy(functions,original->module.functions,(size_t)count*sizeof(*functions));
    memcpy(identities,original->module.declarations->functions,(size_t)count*sizeof(*identities));
    memcpy(generics,original->module.generics,(size_t)count*sizeof(*generics));
    XrXirInstruction ops[2]={{XR_XIR_CONST_INT,XR_XIR_I64,{0},{0},42,{0}},
        {XR_XIR_RETURN,XR_XIR_UNIT,{0},{0},0,{0}}};
    XrXirBlock block={.first=0,.count=2};
    functions[count]=(XrXirFunction){.name="$constant",.name_length=9,.result=XR_XIR_I64,
        .blocks=&block,.block_count=1,.instructions=ops,.instruction_count=2};
    identities[count]=identities[actual[0].helper];
    if (mode==2) identities[count].exported=1;
    if (mode==3) ops[0].op=XR_XIR_ADD_INT;
    XrXirComptimeSite site={actual[0].source_function,actual[0].source_instruction,actual[0].helper};
    CHECK(actual[1].helper==site.helper && actual[1].source_function==site.caller && actual[1].source_instruction==site.instruction);
    XrXirType types[2]={XR_XIR_STRING,XR_XIR_I8};
    XrXirComptimeBinding rows[2]={{0,&types[0],1,count},{0,&types[1],1,count}};
    if (mode==4) types[1]=types[0];
    if (mode==5) ++site.instruction;
    if (mode==6) rows[1].argument_count=0;
    if (mode==7) types[1]=(XrXirType)XR_XIR_TYPE_PARAMETER_BASE;
    XrXirComptimeTable table={&site,1,rows,mode==1?1u:2u};
    XrXirConstructionSpec input={original->construction->rows,original->construction->count,
        original->construction->materializations,original->construction->materialization_count,&table};
    XrXirConstruction *construction=NULL;
    status=xr_xir_compile_construction_new_v3(context,original->module.types,&input,&construction);
    if (status==XR_XIR_OK) {
        /* The receiving owner has copied every vector before producer poison. */
        memset(types,0xa5,sizeof(types));memset(rows,0xa5,sizeof(rows));memset(&site,0xa5,sizeof(site));
        XrXirDeclarations declarations=*original->module.declarations;declarations.functions=identities;
        XrXirModule built=original->module;built.stage=XR_XIR_BUILT;built.provenance=NULL;
        built.functions=functions;built.function_count=count+1;built.generics=generics;built.declarations=&declarations;
        XrXirDiagnostic diagnostic={0};status=xr_xir_compile_check_v2(context,&built,construction,output,&diagnostic);
        if (status!=XR_XIR_OK && !mode) fprintf(stderr,"Binding Built=%u f=%u b=%u i=%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    }
    xr_xir_compile_construction_free(construction);
done:
    xr_compile_resources_free(functions);xr_compile_resources_free(identities);xr_compile_resources_free(generics);
    xr_xir_compile_source_comptime_instances_free(&stage);return status;
}
static void binding_normal(void) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *checked=NULL,*closed=NULL,*lowered=NULL;
    CHECK(binding_source(&context,&checked,0)==XR_XIR_OK && checked);
    const XrXirComptimeTable *table=xr_xir_compile_construction_comptime(checked->construction);
    CHECK(table && table->site_count==1 && table->binding_count==2);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(checked);checked=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&checked,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    XrXirDiagnostic diagnostic={0};XrXirStatus status=xr_xir_compile_specialize(checked,&closed,&diagnostic);
    if (status!=XR_XIR_OK) fprintf(stderr,"Binding specialization=%u f=%u b=%u i=%u\n",status,diagnostic.function,diagnostic.block,diagnostic.instruction);
    CHECK(status==XR_XIR_OK && closed);
    table=xr_xir_compile_construction_comptime(checked->construction);uint32_t edges=0;
    for (uint32_t f=0;f<closed->module.function_count;++f) {
        if (closed->module.provenance->origins[f].function!=table->sites[0].caller) continue;
        const XrXirInstruction *op=&closed->module.functions[f].instructions[table->sites[0].instruction];
        CHECK(op->op==XR_XIR_CALL && closed->module.provenance->origins[op->immediate].function==table->bindings[0].provider);
        ++edges;
    }
    CHECK(edges==2 && !xr_xir_compile_construction_comptime(closed->construction)->site_count);
    XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target,&lowered,NULL)==XR_XIR_OK);
    uint32_t run=named(&lowered->module,"run");XrXirCtfeCallRequest call={.evaluation={.artifact=lowered,.function=run,
        .target=target,.limits=comptime_instances_limits()},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    memcpy(call.evaluation.checked_identity,lowered->checked_identity,32);XrXirValue value={0};
    CHECK(xr_xir_compile_ctfe_calls(&call,&value)==XR_XIR_CTFE_OK && value.type==XR_XIR_I64 && value.payload==42);
    xr_xir_value_drop(&value);xr_xir_compile_artifact_free(closed);xr_xir_compile_artifact_free(checked);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    xr_compile_resources_release(context.resources);
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,run,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    xr_xir_compile_program_drop(program);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Canonical binding real Source two substitutions producer poison/death packet CTFE42 two Instances42 physical0\n");
}
static void binding_controls(void) {
    for (unsigned mode=1;mode<8;++mode) {
        XrXirCompileContext context;context_new(&context);XrXirArtifact *checked=NULL,*closed=NULL;
        XrCompileResourceStats before=stats(&context);XrXirStatus status=binding_source(&context,&checked,mode);
        if (mode==1) {
            CHECK(status==XR_XIR_OK && checked);status=xr_xir_compile_specialize(checked,&closed,NULL);
            CHECK(status==XR_XIR_BAD_TYPE && !closed);
        } else CHECK(status!=XR_XIR_OK && !checked);
        xr_xir_compile_artifact_free(checked);CHECK(stats(&context).live_bytes==before.live_bytes);
        xr_compile_resources_release(context.resources);CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    }
}
