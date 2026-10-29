/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_range_value.c - Real Source and cold Catalog range values and definitions
 */
#include "base/xresource_limits.h"
#include "base/xsha256.h"
#include "shared/xnative_declaration.h"
#include "toolchain/xcompiler_session.h"
#include "xir/xxir_source.h"
#include "xir/xxir_library_catalog.h"
#include "xir/xxir_construction.h"
#include "xir/xxir_nominal.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include "xir/xxir_ctfe.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#c);exit(1);}}while(0)
#include "xir_range_value_cases.h"
#include "xir_range_value_runtime.h"
#include "xir_range_index_runtime.h"
#include "xir_task_result_catalog_cases.h"
#define RANGE_PRELUDE_CANONICAL "stdlib-module-v1:module=7:prelude:path=27:prelude/builtin_symbols.def"
static void range_file(const char *logical,const void *bytes,size_t size) {
    char path[1024];int length=snprintf(path,sizeof(path),"%s/%s",XR_RANGE_VALUE_WORK,logical);
    CHECK(length>0 && (size_t)length<sizeof(path));FILE *file=fopen(path,"wb");
    CHECK(file && fwrite(bytes,1,size,file)==size && !fclose(file));
}
static XrXirStatus range_check(const XrXirCompileContext *context,const char *text,const char *logical,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog,XrXirSourceResult *result) {
    XrCompilerSession *session=NULL;CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_RANGE_VALUE_WORK};char path[1024];
    int length=snprintf(path,sizeof(path),"%s/%s",XR_RANGE_VALUE_WORK,logical);CHECK(length>0 && (size_t)length<sizeof(path));
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,kind,catalog};
    XrXirSourceDiagnostic diagnostic={0};XrXirStatus status;
    if(text) {
        size_t size=strlen(text);char *copy=NULL;
        CHECK(xr_compile_resources_alloc(context->resources,size+1,(void **)&copy)==XR_COMPILE_RESOURCE_OK);
        memcpy(copy,text,size+1);XrXirSourceText input={logical,copy,size};
        status=xr_xir_compile_source_check_text(&request,&input,result,&diagnostic,NULL);
        memset(copy,0xa5,size+1);xr_compile_resources_free(copy);
    } else status=xr_xir_compile_source_check(&request,result,&diagnostic,NULL);
    xr_compile_session_free(session);
    fprintf(stderr,"Range %s Source%u %d:%d %s\n",logical,status,diagnostic.line,diagnostic.column,diagnostic.message);
    if(status!=XR_XIR_OK) CHECK(!result->checked && !result->snapshot && diagnostic.line>0);
    return status;
}
static XrXirSourceResult range_source(const XrXirCompileContext *context,const char *text,const char *logical,
    XrXirLinkageKind kind,const XrXirLibraryCatalog *catalog) {
    XrXirSourceResult result={0};CHECK(range_check(context,text,logical,kind,catalog,&result)==XR_XIR_OK);
    CHECK(result.checked && result.snapshot);return result;
}
static uint32_t range_cancelled_ordinal(const XrXirModule *module) {
    const XrXirNominalTable *table=module->types->nominals;uint32_t found=UINT32_MAX;
    CHECK(table && table->declarations && !table->identities);
    for(uint32_t n=0;n<table->count;++n)if(table->declarations[n].native.native_id==XR_NATIVE_DECLARATION_CANCELLED) {
        CHECK(found==UINT32_MAX);found=n;
    }
    CHECK(found!=UINT32_MAX);return found;
}
static XrXirLibraryCatalog *range_catalog(const XrXirCompileContext *context,const XrXirArtifact *artifact,bool valid) {
    const XrXirModule *module=xr_xir_compile_artifact_module(artifact);uint32_t count=module->declarations->module_count;
    if(valid)(void)range_cancelled_ordinal(module);
    XrXirLibraryModuleInput *bindings=NULL;CHECK(xr_compile_resources_calloc(context->resources,count,sizeof(*bindings),(void **)&bindings)==XR_COMPILE_RESOURCE_OK);
    const XrModuleIdentityAuthority script={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_RANGE_VALUE_WORK};
    char *canonical=NULL;CHECK(xr_compile_module_identity_from_logical(context->resources,&script,"range-lib.xr",&canonical)==XR_MODULE_OK);
    for(uint32_t m=0;m<count;++m) {
        const XrXirSourceModule *actual=&module->declarations->modules[m];
        fprintf(stderr,"Range library module%u root%u %.*s\n",m,module->declarations->root_module,(int)actual->name_length,actual->name);
        if(actual->name_length==strlen(RANGE_PRELUDE_CANONICAL) && !memcmp(actual->name,RANGE_PRELUDE_CANONICAL,actual->name_length))
            bindings[m]=(XrXirLibraryModuleInput){{XR_MODULE_IDENTITY_STDLIB,"prelude",XR_RANGE_VALUE_STDLIB},"prelude/builtin_symbols.def"};
        else {CHECK(actual->name_length==strlen(canonical) && !memcmp(actual->name,canonical,actual->name_length));
            bindings[m]=(XrXirLibraryModuleInput){script,"range-lib.xr"};}
    }
    xr_compile_resources_free(canonical);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(artifact,&packet,NULL)==XR_XIR_OK);
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,.modules=bindings,.module_count=count};
    xr_sha256(packet.bytes,packet.length,input.sha256);XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    fprintf(stderr,"Range Catalog status%u expected-valid%u\n",status,valid?1u:0u);
    if(valid && status==XR_XIR_OK)task_catalog_rejections(context,&packet,&input);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_compile_resources_free(bindings);
    CHECK(valid?(status==XR_XIR_OK && catalog):(status==XR_XIR_BAD_STRUCTURE && !catalog));return catalog;
}
static uint32_t range_entry(const XrXirModule *module,const char *name) {
    uint32_t found=UINT32_MAX;
    for(uint32_t f=0;f<module->function_count;++f) {
        const XrXirFunction *body=&module->functions[f];const XrXirFunctionIdentity *id=&module->declarations->functions[f];
        if(id->module==module->declarations->root_module && !id->nominal_owner && body->name_length==strlen(name) &&
            !memcmp(body->name,name,body->name_length)){CHECK(found==UINT32_MAX && id->exported);found=f;}
    }
    CHECK(found!=UINT32_MAX);return found;
}
static void range_index_structure(const XrXirCompileContext *context,const XrXirArtifact *artifact) {
    const XrXirModule *original=xr_xir_compile_artifact_module(artifact);uint32_t selected=UINT32_MAX,index=UINT32_MAX;
    for(uint32_t f=0;f<original->function_count;++f) {
        if(original->functions[f].name_length!=7 || memcmp(original->functions[f].name,"indexAt",7))continue;
        for(uint32_t i=0;i<original->functions[f].instruction_count;++i)
        if(original->functions[f].instructions[i].op==XR_XIR_INDEX_CHECK){selected=f;index=i;break;}
    }
    CHECK(selected!=UINT32_MAX && index!=UINT32_MAX);
    XrXirFunction *functions=NULL;XrXirInstruction *instructions=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,(size_t)original->function_count*sizeof(*functions),(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    memcpy(functions,original->functions,(size_t)original->function_count*sizeof(*functions));
    XrXirFunction *body=&functions[selected];
    CHECK(body->parameter_count==2 && body->parameters[0]!=XR_XIR_I64 && body->parameters[1]==XR_XIR_I64);
    CHECK(xr_compile_resources_alloc(context->resources,(size_t)body->instruction_count*sizeof(*instructions),(void **)&instructions)==XR_COMPILE_RESOURCE_OK);
    body->instructions=instructions;XrXirModule altered=*original;altered.functions=functions;
    for(unsigned mode=0;mode<11;++mode) {
        memcpy(instructions,original->functions[selected].instructions,(size_t)body->instruction_count*sizeof(*instructions));
        XrXirInstruction *op=&instructions[index];
        if(mode==0)op->immediate=1;
        else if(mode<3)op->targets[mode-1]=1;
        else if(mode<5)op->type_arguments[mode-3]=1;
        else if(mode==5)op->type=XR_XIR_I64;
        else if(mode==6)op->args[0]=body->parameter_count+index;
        else if(mode==7)op->args[1]=body->parameter_count+index;
        else if(mode==8)op->args[0]=UINT32_MAX;
        else op->args[mode-9]=0;
        XrXirArtifact *rejected=NULL;XrXirDiagnostic diagnostic={0};
        CHECK(xr_xir_compile_recheck_v2(context,&altered,xr_xir_compile_artifact_construction(artifact),&rejected,&diagnostic)!=XR_XIR_OK);
        CHECK(!rejected && diagnostic.function==selected && diagnostic.instruction==index);
    }
    xr_compile_resources_free(instructions);xr_compile_resources_free(functions);
    fprintf(stderr,"Range INDEX_CHECK: eleven payload/shape/type/SSA attacks rejected at exact guard\n");
}
static void range_previous_contract(const XrXirCompileContext *context,XrXirCheckedPacket *packet) {
    CHECK(packet->length>64 && packet->bytes[12]==107 && !packet->bytes[13] && !packet->bytes[14] && !packet->bytes[15]);
    XrCompileResourceStats before={0},after={0};
    CHECK(xr_compile_resources_stats(context->resources,&before)==XR_COMPILE_RESOURCE_OK);
    packet->bytes[12]=106;XrXirArtifact *rejected=NULL;
    CHECK(xr_xir_compile_checked_read(context,packet->bytes,packet->length,&rejected,NULL)==XR_XIR_BAD_STRUCTURE && !rejected);
    packet->bytes[12]=107;
    CHECK(xr_compile_resources_stats(context->resources,&after)==XR_COMPILE_RESOURCE_OK && before.held_bytes==after.held_bytes);
}
static void range_index_ctfe(const XrXirArtifact *closed,const XrXirArtifact *lowered) {
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    XrXirCtfeCallRequest request={.evaluation={.artifact=lowered,
        .target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},
        .call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    xr_sha256(packet.bytes,packet.length,request.evaluation.checked_identity);xr_xir_compile_checked_packet_free(&packet);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t functions[]={range_entry(module,"indexed"),range_entry(module,"indexedCode")};
    for(size_t v=0;v<sizeof(range_examples)/sizeof(*range_examples);++v) {
        XrRangeCore oracle=xr_range_core_make_with_bound(range_examples[v].start,range_examples[v].end,1,range_examples[v].closed);
        int64_t length=xr_range_core_length(oracle);const int64_t keys[]={INT64_MIN,-1,0,1,length?length-1:0,length,INT64_MAX};
        for(size_t k=0;k<sizeof(keys)/sizeof(*keys);++k)for(unsigned caught=0;caught<2;++caught) {
            bool valid=false;int64_t expected=xr_range_core_index(oracle,keys[k],&valid);
            XrXirValue args[]={{XR_XIR_I64,0,oracle.start},{XR_XIR_I64,0,oracle.end},{XR_XIR_BOOL,0,oracle.inclusive_end},{XR_XIR_I64,0,keys[k]}};
            request.evaluation.function=functions[caught];request.evaluation.arguments=args;request.evaluation.argument_count=4;
            XrXirValue value={0};XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&request,&value);
            if(valid || caught){CHECK(status==XR_XIR_CTFE_OK);range_integer(&value,valid?expected:430);}
            else CHECK(status==XR_XIR_CTFE_BOUNDS && !value.type && !value.reserved && !value.payload);
        }
    }
    fprintf(stderr,"Range INDEX_CHECK CTFE: full integer extremes, uncaught bounds and local panic430 PASS\n");
}
static void range_scalar_guard(const char *c_path) {
    XrXirCompileContext context=range_context();
    const char *text="export fn rawIndex(index:i64,length:i64)->i64{return index+length}\n"
        "export fn rawProtected(index:i64,length:i64)->i64{try{return index+length}catch panic(p){return p.code}}\n";
    XrXirSourceResult source=range_source(&context,text,"scalar-index.xr",XR_XIR_PROGRAM,NULL);
    const XrXirModule *original=xr_xir_compile_artifact_module(source.checked);
    uint32_t entries[]={range_entry(original,"rawIndex"),range_entry(original,"rawProtected")};
    XrXirFunction *functions=NULL;XrXirInstruction *owned[2]={0};
    CHECK(xr_compile_resources_alloc(context.resources,(size_t)original->function_count*sizeof(*functions),(void **)&functions)==XR_COMPILE_RESOURCE_OK);
    memcpy(functions,original->functions,(size_t)original->function_count*sizeof(*functions));
    for(unsigned selected=0;selected<2;++selected) {
        XrXirFunction *body=&functions[entries[selected]];XrXirInstruction *instructions=NULL;uint32_t guard=UINT32_MAX;bool returned=false;
        CHECK(body->parameter_count==2 && xr_compile_resources_alloc(context.resources,(size_t)body->instruction_count*sizeof(*instructions),(void **)&instructions)==XR_COMPILE_RESOURCE_OK);
        memcpy(instructions,body->instructions,(size_t)body->instruction_count*sizeof(*instructions));body->instructions=owned[selected]=instructions;
        for(uint32_t i=0;i<body->instruction_count;++i) {
            XrXirInstruction *op=&instructions[i];
            if(op->op==XR_XIR_ADD_INT) {CHECK(guard==UINT32_MAX);guard=body->parameter_count+i;*op=(XrXirInstruction){.op=XR_XIR_INDEX_CHECK,.type=XR_XIR_UNIT,.args={0,1}};}
            if(op->op==XR_XIR_RETURN && op->args[0]==guard){CHECK(!returned);op->args[0]=0;returned=true;}
        }
        CHECK(guard!=UINT32_MAX && returned);
    }
    XrXirModule altered=*original;altered.functions=functions;XrXirArtifact *raw=NULL;
    CHECK(xr_xir_compile_recheck_v2(&context,&altered,xr_xir_compile_artifact_construction(source.checked),&raw,NULL)==XR_XIR_OK);
    xr_compile_resources_free(owned[0]);xr_compile_resources_free(owned[1]);xr_compile_resources_free(functions);xr_xir_compile_source_result_free(&source);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(raw,&packet,NULL)==XR_XIR_OK);range_previous_contract(&context,&packet);
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);xr_xir_compile_artifact_free(raw);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    CHECK(xr_xir_compile_checked_write(closed,&packet,NULL)==XR_XIR_OK);
    XrXirCtfeCallRequest request={.evaluation={.target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION},
        .limits={XR_XIR_CTFE_MAX_STEPS,XR_XIR_CTFE_MAX_FRAME_BYTES,XR_XIR_CTFE_MAX_OUTPUT_BYTES}},.call_depth=XR_XIR_CTFE_MAX_CALL_DEPTH};
    xr_sha256(packet.bytes,packet.length,request.evaluation.checked_identity);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_lower(closed,&request.evaluation.target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    request.evaluation.artifact=lowered;
    for(size_t i=0;i<sizeof(range_index_inputs)/sizeof(*range_index_inputs);++i)for(unsigned caught=0;caught<2;++caught) {
        int64_t index=range_index_inputs[i].index,length=range_index_inputs[i].length;
        XrXirValue args[]={{XR_XIR_I64,0,index},{XR_XIR_I64,0,length}},value={0};
        request.evaluation.function=entries[caught];request.evaluation.arguments=args;request.evaluation.argument_count=2;
        XrXirCtfeStatus status=xr_xir_compile_ctfe_calls(&request,&value);
        if(length<0)CHECK(status==XR_XIR_CTFE_BAD_ARTIFACT && !value.type && !value.payload);
        else if(!caught && (index<0 || index>=length))CHECK(status==XR_XIR_CTFE_BOUNDS && !value.type && !value.payload);
        else {CHECK(status==XR_XIR_CTFE_OK);range_integer(&value,index<0 || index>=length?430:index);}
    }
    if(c_path) {
        XrXirCSource c={0};CHECK(xr_xir_compile_emit_c(lowered,"range_index",SIZE_MAX,&c)==XR_XIR_OK);
        FILE *file=fopen(c_path,"wb");CHECK(file && fwrite(c.text,1,c.length,file)==c.length);
        CHECK(fprintf(file,"\nconst uint32_t range_index_selected[2]={%uu,%uu};\n",entries[0],entries[1])>0 && !fclose(file));xr_xir_compile_c_source_free(&c);
    }
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    range_index_runtime(program,entries);xr_xir_compile_program_drop(program);xr_compile_resources_release(context.resources);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Scalar INDEX_CHECK: raw/codec/VM/CTFE valid,430 and negative length engine failure PASS\n");
}
static void range_definition_negatives(const XrXirCompileContext *context,const XrXirArtifact *producer) {
    const XrXirModule *original=xr_xir_compile_artifact_module(producer);uint32_t ctor=UINT32_MAX,count=UINT32_MAX;
    uint32_t cancelled=range_cancelled_ordinal(original);
    XrXirTypes types=*original->types;XrXirNominalTable table=*types.nominals;
    XrXirNominalDeclaration *records=NULL;
    CHECK(xr_compile_resources_alloc(context->resources,(size_t)table.count*sizeof(*records),(void **)&records)==XR_COMPILE_RESOURCE_OK);
    table.declarations=records;types.nominals=&table;
    for(unsigned mode=0;mode<4;++mode) {
        memcpy(records,original->types->nominals->declarations,(size_t)table.count*sizeof(*records));
        XrXirNominalDeclaration *record=&records[cancelled];
        XrXirNominalVariant variant=record->variants[0];
        XrXirNominalField payload={.name={"value",5},.type=XR_XIR_I64};
        if(!mode) {
            const XrXirSourceModule *script=&original->declarations->modules[0];
            CHECK(script->name_length!=record->module.length || memcmp(script->name,record->module.bytes,script->name_length));
            record->module=(XrXirLiteral){script->name,script->name_length};
        } else if(mode==1)record->native.source_fingerprint[0]^=1;
        else if(mode==2) {
            record->fields=&payload;record->field_count=1;variant.field_count=1;record->variants=&variant;
        } else record->native.native_id=UINT32_MAX;
        CHECK(xr_xir_compile_nominal_structure_verify(context,&table,&types)==XR_XIR_BAD_STRUCTURE);
        XrXirModule altered=*original;altered.types=&types;XrXirArtifact *owned=NULL;
        CHECK(xr_xir_compile_recheck_v2(context,&altered,xr_xir_compile_artifact_construction(producer),&owned,NULL)==XR_XIR_BAD_STRUCTURE && !owned);
    }
    xr_compile_resources_free(records);
    fprintf(stderr,"Range Cancelled authentication: origin/fingerprint/payload/unknown ID rejected\n");
    for(uint32_t f=0;f<original->function_count;++f) {
        uint32_t owner=original->declarations->functions[f].nominal_owner;if(!owner)continue;
        const XrXirNominalDeclaration *d=&original->types->nominals->declarations[owner-1];
        if(d->name.length!=5 || memcmp(d->name.bytes,"Range",5))continue;
        const XrXirFunction *body=&original->functions[f];
        if(body->name_length==4 && !memcmp(body->name,"init",4))ctor=f;
        if(body->name_length==5 && !memcmp(body->name,"count",5))count=f;
    }
    CHECK(ctor!=UINT32_MAX && count!=UINT32_MAX);
    for(unsigned mode=0;mode<2;++mode) {
        XrXirFunction *functions=NULL;CHECK(xr_compile_resources_alloc(context->resources,
            (size_t)original->function_count*sizeof(*functions),(void **)&functions)==XR_COMPILE_RESOURCE_OK);
        memcpy(functions,original->functions,(size_t)original->function_count*sizeof(*functions));
        XrXirInstruction *instructions=NULL;uint32_t operands[4]={13,12,14,15};
        if(!mode){CHECK(functions[ctor].operand_count==4);functions[ctor].operands=operands;}
        else {
            XrXirFunction *body=&functions[count];CHECK(xr_compile_resources_alloc(context->resources,
                (size_t)body->instruction_count*sizeof(*instructions),(void **)&instructions)==XR_COMPILE_RESOURCE_OK);
            memcpy(instructions,body->instructions,(size_t)body->instruction_count*sizeof(*instructions));body->instructions=instructions;
            bool changed=false;for(uint32_t i=0;i<body->instruction_count;++i)if(instructions[i].op==XR_XIR_CONST_INT && instructions[i].immediate==INT64_MAX){instructions[i].immediate=INT64_MAX-1;changed=true;}
            CHECK(changed);
        }
        XrXirModule altered=*original;altered.functions=functions;XrXirArtifact *owned=NULL;
        CHECK(xr_xir_compile_recheck_v2(context,&altered,xr_xir_compile_artifact_construction(producer),&owned,NULL)==XR_XIR_OK);
        CHECK(!range_catalog(context,owned,false));xr_xir_compile_artifact_free(owned);
        xr_compile_resources_free(instructions);xr_compile_resources_free(functions);
    }
}
static void range_authority(void) {
    XrXirCompileContext context=range_context();
    const char *positive="type R=Range\nexport fn f(r:R)->i64{return r.start+r.end+r.count()+len(r)}";
    XrXirSourceResult result=range_source(&context,positive,"typed.xr",XR_XIR_PROGRAM,NULL);xr_xir_compile_source_result_free(&result);
    result=range_source(&context,range_constructor_source,"generic.xr",XR_XIR_PROGRAM,NULL);
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(result.checked,&closed,NULL)==XR_XIR_OK);xr_xir_compile_source_result_free(&result);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint32_t entry=range_entry(xr_xir_compile_artifact_module(lowered),"generic");
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
    XrXirValue answer=range_take(instance,entry,NULL,0);range_integer(&answer,60);
    xr_xir_compile_program_drop(program);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    for(size_t i=0;i<sizeof(range_rejections)/sizeof(*range_rejections);++i) {
        XrXirSourceResult refused={0};CHECK(range_check(&context,range_rejections[i],"refused.xr",XR_XIR_PROGRAM,NULL,&refused)!=XR_XIR_OK);
        xr_xir_compile_source_result_free(&refused);
    }
    result=range_source(&context,range_library_source,"range-lib.xr",XR_XIR_LIBRARY,NULL);
    range_definition_negatives(&context,result.checked);xr_xir_compile_source_result_free(&result);
    xr_compile_resources_release(context.resources);
}
static void range_flow(unsigned mode,const char *packet_path) {
    XrXirCompileContext context=range_context();XrXirLibraryCatalog *catalog=NULL;
    range_file("range-lib.xr",range_library_source,strlen(range_library_source));
    if(mode==2) {
        XrXirSourceResult producer=range_source(&context,range_library_source,"range-lib.xr",XR_XIR_LIBRARY,NULL);
        catalog=range_catalog(&context,producer.checked,true);xr_xir_compile_source_result_free(&producer);
    }
    const char *logical=mode==1?"entry-file.xr":"receiver.xr";
    if(mode==1)range_file(logical,range_consumer_source,strlen(range_consumer_source));
    XrXirSourceResult source=range_source(&context,mode==1?NULL:range_consumer_source,logical,XR_XIR_PROGRAM,catalog);
    xr_xir_compile_library_catalog_free(catalog);
    range_index_structure(&context,source.checked);
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source.checked,&packet,NULL)==XR_XIR_OK);
    range_previous_contract(&context,&packet);
    xr_xir_compile_source_result_free(&source);
    if(packet_path){FILE *file=fopen(packet_path,"wb");CHECK(file && fwrite(packet.bytes,1,packet.length,file)==packet.length && !fclose(file));}
    XrXirArtifact *received=NULL,*closed=NULL,*lowered=NULL;CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);
    range_index_ctfe(closed,lowered);
    xr_xir_compile_artifact_free(closed);const XrXirModule *module=xr_xir_compile_artifact_module(lowered);uint32_t selection[RANGE_SELECTION_COUNT];
    for(unsigned f=0;f<RANGE_SELECTION_COUNT;++f)selection[f]=range_entry(module,range_entry_names[f]);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirValue kept=range_runtime_cases(program,selection);xr_compile_resources_release(context.resources);range_retained(&kept);
    CHECK(!runtime_live && !runtime_bytes && !source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Range flow%u complete: actual fields/count/len, once, all producer deaths\n",mode);
}
static void task_result_catalog(void) {
    XrXirCompileContext context=range_context();
    range_file("range-lib.xr",task_catalog_library_source,strlen(task_catalog_library_source));
    XrXirSourceResult producer=range_source(&context,task_catalog_library_source,"range-lib.xr",XR_XIR_LIBRARY,NULL);
    XrXirLibraryCatalog *catalog=range_catalog(&context,producer.checked,true);
    xr_xir_compile_source_result_free(&producer);
    range_file("range-lib.xr","unavailable",11);
    XrXirSourceResult source=range_source(&context,task_catalog_consumer_source,"task-catalog.xr",XR_XIR_PROGRAM,catalog);
    xr_xir_compile_library_catalog_free(catalog);
    XrXirArtifact *closed=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_specialize(source.checked,&closed,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&source);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target,NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    const char *names[]={"success","failure","panicked","cancelled"};uint32_t functions[4];
    for(unsigned n=0;n<4;++n)functions[n]=range_entry(xr_xir_compile_artifact_module(lowered),names[n]);
    XrXirProgram *program=NULL;CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);
    XrXirInstanceConfig options;CHECK(xr_xir_instance_config_init(&options,sizeof(options))==XR_XIR_CALL_READY);
    XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&options,&instance)==XR_XIR_CALL_READY);
    XrXirValue kept[4]={0};
    for(unsigned n=0;n<4;++n) {
        CHECK(xr_xir_instance_start(instance,functions[n],NULL,0)==XR_XIR_CALL_READY);
        XrXirInstanceResult result={0};unsigned polls=0;
        do {CHECK(++polls<1024);result=xr_xir_instance_poll_bounded(instance,64);}while(result.outcome.status==XR_XIR_CALL_READY);
        CHECK(result.outcome.status==XR_XIR_CALL_RETURNED);
        CHECK(xr_xir_instance_take_result(instance,&kept[n])==XR_XIR_CALL_RETURNED);
    }
    xr_xir_compile_program_drop(program);CHECK(xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    xr_compile_resources_release(context.resources);
    const char *members[]={"Success","Error","Panic","Cancelled"};
    for(unsigned n=0;n<4;++n) {
        uint32_t tag=UINT32_MAX;CHECK(xr_xir_enum_variant(&kept[n],&tag)==XR_XIR_VALUE_OK && tag==n);
        XrXirEnumBorrow borrowed={0};CHECK(xr_xir_enum_borrow(&kept[n],&borrowed)==XR_XIR_VALUE_OK);
        CHECK(borrowed.name.length==10 && !memcmp(borrowed.name.bytes,"TaskResult",10));
        CHECK(borrowed.member.length==strlen(members[n]) && !memcmp(borrowed.member.bytes,members[n],borrowed.member.length));
        CHECK(borrowed.field_count==(n<3?1u:0u));
        if(n<3)CHECK(borrowed.fields[0].type==(uint32_t)(n==0?XR_XIR_I64:n==1?XR_XIR_ERROR:XR_XIR_PANIC_INFO));
        if(!n)CHECK(borrowed.fields[0].payload==42);
        if(n==1 || n==2)task_catalog_nested(&borrowed.fields[0],n);
        xr_xir_value_drop(&kept[n]);
    }
    fprintf(stderr,"Cold TaskResult Catalog: four owned terminal payloads survive every producer and program\n");
}
int main(int argc,char **argv) {
    if(argc==3 && !strcmp(argv[1],"--index-raw")){range_scalar_guard(argv[2]);return 0;}
    if(argc==2 && !strcmp(argv[1],"--authority")){range_authority();return 0;}
    CHECK(argc==1 || argc==3);
    range_scalar_guard(NULL);
    range_flow(0,argc==3?argv[1]:NULL);range_flow(1,NULL);range_flow(2,argc==3?argv[2]:NULL);
    task_result_catalog();return 0;
}
