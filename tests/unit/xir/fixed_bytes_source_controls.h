/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fixed_bytes_source_controls.h - Exact annotations and owned cold library bytes
 *
 * KEY CONCEPT:
 *   An authenticated cold library retains the fixed length without source files.
 */
#include "xir/xxir_library_catalog.h"
#include "base/xsha256.h"
#include "base/xcompile_request.h"
#include "xir/xxir_library_catalog.c"
static uint64_t fixed_held(const XrXirCompileContext *context) {
    XrCompileResourceStats stats={0};
    CHECK(xr_compile_resources_stats(context->resources,&stats)==XR_COMPILE_RESOURCE_OK);
    return stats.held_bytes;
}
typedef struct FixedBytesCancel {uint32_t polls,stop;} FixedBytesCancel;
static bool fixed_cancel_at(const void *owner) {
    FixedBytesCancel *state=(FixedBytesCancel *)owner;return ++state->polls==state->stop;
}
static XrXirStatus fixed_source_text(const XrXirCompileContext *context,const char *text,
    const char *name,XrXirLinkageKind linkage,const XrXirLibraryCatalog *catalog,
    XrCompileRequestControl *control,XrXirSourceResult *output) {
    char path[2048];int size=snprintf(path,sizeof(path),"%s/%s",XR_FIXED_BYTES_ROOT,name);
    CHECK(size>0 && (size_t)size<sizeof(path));XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(context->resources,&session)==XR_COMPILE_RESOURCE_OK);
    if(control)CHECK(xr_compile_session_bind_request(session,control)==XR_COMPILE_RESOURCE_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_FIXED_BYTES_ROOT};
    XrXirSourceRequest request={.session=session,.entry_path=path,.authority=&authority,
        .context=context,.linkage_kind=linkage,.libraries=catalog};
    XrXirSourceText input={name,text,strlen(text)};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_text(&request,&input,output,&diagnostic,NULL);
    if(status!=XR_XIR_OK)fprintf(stderr,"fixed text status%u %d:%d %s\n",status,
        diagnostic.line,diagnostic.column,diagnostic.message);
    xr_compile_session_free(session);return status;
}
static void fixed_source_controls(const XrXirCompileContext *context) {
    const char *sources[]={
        "const x:[u8;2]=b\"AB\"\nassert(len(x)==2 && x[0]==65)\n",
        "const x:[u8;3]=b\"AB\"\n",
        "const x: Array<u8> = b\"AB\"\n",
        "const x:[i64;2]=b\"AB\"\n",
        "const x=b\"\"\n",
        "const x:[u8;0]=b\"A\"\n",
        "const x:[u8;65536]=b\"A\"\n",
        "var x=b\"AB\"\nx[0]=1\n",
        "const x=c\"A\\0B\"\n"};
    uint64_t held=fixed_held(context);
    for(unsigned i=0;i<sizeof(sources)/sizeof(*sources);++i) {
        XrXirSourceResult result={0};
        XrXirStatus status=fixed_source_text(context,sources[i],"fixed-control.xr",
            XR_XIR_PROGRAM,NULL,NULL,&result);
        CHECK((status==XR_XIR_OK)==!i && (result.checked!=NULL)==!i);
        xr_xir_compile_source_result_free(&result);CHECK(fixed_held(context)==held);
    }
    FixedBytesCancel cancelled={0,1};XrCompileRequestControl control={0};
    XrCompileRequestOptions options={fixed_cancel_at,&cancelled,XR_COMPILE_DEADLINE_NONE,0};
    CHECK(xr_compile_request_control_init(&options,&control)==XR_COMPILE_RESOURCE_OK);
    XrXirSourceResult result={0};
    CHECK(fixed_source_text(context,sources[0],"fixed-control.xr",XR_XIR_PROGRAM,NULL,
        &control,&result)==XR_XIR_CANCELLED && !result.checked && cancelled.polls);
    CHECK(fixed_held(context)==held);
    XrXirCompileContext bounded=*context;bounded.limits.instructions=1;
    CHECK(fixed_source_text(&bounded,sources[0],"fixed-control.xr",XR_XIR_PROGRAM,NULL,
        NULL,&result)==XR_XIR_BUDGET && !result.checked && fixed_held(context)==held);
    CHECK(fixed_source_text(context,sources[0],"fixed-control.xr",XR_XIR_PROGRAM,NULL,
        NULL,&result)==XR_XIR_OK && result.checked);
    xr_xir_compile_source_result_free(&result);CHECK(fixed_held(context)==held);
}
static void fixed_sendable_controls(const XrXirCompileContext *context) {
    const XrXirType type=(XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE;
    XrXirTypeNode node={.kind=XR_XIR_TYPE_FIXED_ARRAY,.element=XR_XIR_U8,.parameter_count=4};
    const XrXirTypes types={&node,1,NULL,NULL};
    const XrXirModule module={.stage=XR_XIR_BUILT,.types=&types};
    const XrXirProofContext proof={&module,{XR_XIR_CONTEXT_CLOSED,0,0}};
    const XrXirTypeNode original=node;uint64_t held=fixed_held(context);
    const uint32_t lengths[]={1,4,65535};
    for(uint32_t i=0;i<sizeof(lengths)/sizeof(*lengths);++i) {
        node.parameter_count=lengths[i];
        CHECK(xr_xir_compile_types_structure_verify(context,&types)==XR_XIR_OK);
        CHECK(xr_xir_compile_type_use_verify(context,&proof,type)==XR_XIR_OK);
        CHECK(xr_xir_compile_type_markers_prove(context,&proof,type,XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
        CHECK(fixed_held(context)==held);
    }
    XrXirCallableParameter parameter={XR_XIR_U8,0};XrXirType nominal_argument=XR_XIR_U8;
    for(unsigned attack=0;attack<10;++attack) {
        node=original;
        if(attack==0)node.parameter_count=0;
        if(attack==1)node.parameter_count=65536;
        if(attack==2)node.element=XR_XIR_I64;
        if(attack==3)node.parameters=&parameter;
        if(attack==4)node.result=XR_XIR_I64;
        if(attack==5)node.flags=1;
        if(attack==6)node.parameter_span=1;
        if(attack==7)node.nominal.declaration=1;
        if(attack==8){node.nominal.arguments=&nominal_argument;node.nominal.argument_count=1;}
        if(attack==9){node.nominal.fields=&nominal_argument;node.nominal.field_count=1;}
        XrXirStatus expected=attack>=7?XR_XIR_BAD_STRUCTURE:XR_XIR_BAD_TYPE;
        CHECK(xr_xir_compile_type_markers_prove(context,&proof,type,XR_XIR_CONSTRAINT_SENDABLE)==expected);
        CHECK(fixed_held(context)==held);node=original;
        CHECK(xr_xir_compile_type_markers_prove(context,&proof,type,XR_XIR_CONSTRAINT_SENDABLE)==XR_XIR_OK);
    }
    CHECK(xr_xir_compile_type_markers_prove(context,&proof,type,XR_XIR_CONSTRAINT_EQUAL)==XR_XIR_BAD_TYPE);
    CHECK(fixed_held(context)==held);
}
/* These are private negative probes over copies of real verified declarations,
 * not alternate Source programs or substitutes for packet reception. */
static void fixed_cold_slot_refusal(const XrXirModule *module,
    const XrXirConstruction *construction,bool cell) {
    CHECK(module && module->declarations && construction);
    const XrXirDeclarations *original=module->declarations;
    CHECK(original->slot_count && original->slot_count<=2 && original->slots);
    const XrXirSlotFacts *facts=xr_xir_compile_construction_slots(construction);
    CHECK(facts && facts->count==original->slot_count && facts->rows);
    XrXirSlot slots[2];memcpy(slots,original->slots,original->slot_count*sizeof(*slots));
    XrXirDeclarations copy=*original;copy.slots=slots;uint32_t ordinal=UINT32_MAX;
    for(uint32_t s=0;s<original->slot_count;++s) {
        const XrXirTypeNode *node=xr_xir_type_node(module->types,slots[s].type);
        if(cell) {
            if(!node || node->kind!=XR_XIR_TYPE_CELL || slots[s].mutable!=1)continue;
            node=xr_xir_type_node(module->types,node->element);
        } else if(slots[s].mutable)continue;
        if(node && node->kind==XR_XIR_TYPE_FIXED_ARRAY && node->element==XR_XIR_U8) {
            ordinal=s;break;
        }
    }
    CHECK(ordinal!=UINT32_MAX && facts->rows[ordinal].slot==ordinal);
    const XrXirSlot saved=slots[ordinal];slots[ordinal].mutable=cell?0:1;
    CHECK(!library_catalog_state_slot(module->types,&copy,facts,ordinal));
    CHECK(!memcmp(&original->slots[ordinal],&saved,sizeof(saved)));
}
static void fixed_cold_state_refusal(const XrXirCompileContext *context) {
    uint64_t held=fixed_held(context);XrXirSourceResult producer={0};
    CHECK(fixed_source_text(context,"var blocked=b\"\\x00\\xFF\"\n"
        "export fn readFixed()->[u8;2]{return blocked}\n",
        "fixed-state.xr",XR_XIR_LIBRARY,NULL,NULL,&producer)==XR_XIR_OK);
    const XrXirModule *module=xr_xir_compile_artifact_module(producer.checked);
    CHECK(module && module->declarations && module->declarations->slot_count==1);
    const XrXirSlot *slot=&module->declarations->slots[0];
    const XrXirTypeNode *cell=xr_xir_type_node(module->types,slot->type);
    CHECK(slot->mutable && cell && cell->kind==XR_XIR_TYPE_CELL);
    const XrXirTypeNode *fixed=xr_xir_type_node(module->types,cell->element);
    CHECK(fixed && fixed->kind==XR_XIR_TYPE_FIXED_ARRAY && fixed->element==XR_XIR_U8 &&
        fixed->parameter_count==2);
    CHECK(xr_xir_compile_artifact_verify(producer.checked,NULL)==XR_XIR_OK);
    fixed_cold_slot_refusal(module,xr_xir_compile_artifact_construction(producer.checked),true);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&producer);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_FIXED_BYTES_ROOT};
    XrXirLibraryModuleInput binding={authority,"fixed-state.xr"};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,
        .modules=&binding,.module_count=1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    if(status!=XR_XIR_BAD_STAGE)fprintf(stderr,"fixed mutable Cell catalog status%u\n",(unsigned)status);
    CHECK(status==XR_XIR_BAD_STAGE && !catalog);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(fixed_held(context)==held);
}
static void fixed_cold_library(const XrXirCompileContext *context,FILE *generated) {
    uint64_t held=fixed_held(context);XrXirSourceResult producer={0};
    CHECK(fixed_source_text(context,"const hidden=b\"AB\"\n"
        "const transferred=b\"\\x00\\xFF\\x41\\x80\"\n"
        "export fn magic()->[u8;2]{return hidden}\n"
        "export fn makeBytes()->[u8;4]{return transferred}\n",
        "fixed-lib.xr",XR_XIR_LIBRARY,NULL,NULL,&producer)==XR_XIR_OK);
    XrXirCheckedPacket packet={0};
    CHECK(xr_xir_compile_checked_write(producer.checked,&packet,NULL)==XR_XIR_OK);
    fixed_cold_slot_refusal(xr_xir_compile_artifact_module(producer.checked),
        xr_xir_compile_artifact_construction(producer.checked),false);
    xr_xir_compile_source_result_free(&producer);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,XR_FIXED_BYTES_ROOT};
    XrXirLibraryModuleInput binding={authority,"fixed-lib.xr"};
    XrXirLibraryInput input={.packet=packet.bytes,.length=packet.length,
        .modules=&binding,.module_count=1};xr_sha256(packet.bytes,packet.length,input.sha256);
    XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus catalog_status=xr_xir_compile_library_catalog_new_v2(context,&input,1,&catalog);
    if(catalog_status!=XR_XIR_OK) {
        static const char *const names[]={"XR_XIR_OK","XR_XIR_BAD_STRUCTURE","XR_XIR_BAD_STAGE",
            "XR_XIR_BAD_TYPE","XR_XIR_BAD_VALUE","XR_XIR_BAD_DOMINANCE","XR_XIR_BUDGET",
            "XR_XIR_OUT_OF_MEMORY","XR_XIR_BAD_LAYOUT","XR_XIR_IO","XR_XIR_UNRESOLVED",
            "XR_XIR_UNSUPPORTED","XR_XIR_MEMORY_LIMIT","XR_XIR_OVERFLOW","XR_XIR_CANCELLED",
            "XR_XIR_DEADLINE","XR_XIR_CLOCK_FAILURE"};
        _Static_assert(sizeof(names)/sizeof(*names)==XR_XIR_CLOCK_FAILURE+1,"catalog status names match the public enum");
        fprintf(stderr,"fixed cold stage=catalog_new_v2 status=%u (%s)\n",(unsigned)catalog_status,
            (unsigned)catalog_status<sizeof(names)/sizeof(*names)?names[catalog_status]:"unknown XrXirStatus");
    }
    CHECK(catalog_status==XR_XIR_OK);
    fixed_cold_state_refusal(context);
    memset(packet.bytes,0xcc,packet.length);xr_xir_compile_checked_packet_free(&packet);
    XrXirSourceResult consumer={0};
    CHECK(fixed_source_text(context,"import {magic,makeBytes} from \"./fixed-lib\"\n"
        "const x=magic()\nassert(len(x)==2 && x[1]==66)\n"
        "export fn fixedMake()->[u8;4]{return makeBytes()}\n"
        "export fn fixedRead(value:[u8;4])->i64{\n"
        "assert(len(value)==4 && value[0]==0 && value[1]==255 && value[2]==65 && value[3]==128)\n"
        "return value[2] as i64}\n"
        "export fn fixedClone(value:[u8;4])->[u8;4]{const copy=value\nreturn copy}\n",
        "fixed-main.xr",XR_XIR_PROGRAM,catalog,NULL,&consumer)==XR_XIR_OK);
    xr_xir_compile_library_catalog_free(catalog);catalog=NULL;
    XrXirCheckedPacket received={0};
    CHECK(xr_xir_compile_checked_write(consumer.checked,&received,NULL)==XR_XIR_OK);
    xr_xir_compile_source_result_free(&consumer);
    XrXirArtifact *cold=NULL,*special=NULL,*lowered=NULL;
    CHECK(xr_xir_compile_checked_read(context,received.bytes,received.length,&cold,NULL)==XR_XIR_OK);
    memset(received.bytes,0xcc,received.length);xr_xir_compile_checked_packet_free(&received);
    CHECK(xr_xir_compile_specialize(cold,&special,NULL,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(cold);
    CHECK(xr_xir_compile_lower(special,&(XrXirTarget){XR_XIR_ARCH_LE64,XR_XIR_VALUE_ABI_VERSION},
        NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(special);
    const XrXirModule *module=xr_xir_compile_artifact_module(lowered);
    uint32_t ids[4]={module->declarations->entry_function,UINT32_MAX,UINT32_MAX,UINT32_MAX};
    static const char *const names[]={"fixedMake","fixedRead","fixedClone"};
    for(uint32_t n=0;n<3;++n) {
        for(uint32_t f=0;f<module->function_count;++f) {
            const XrXirFunction *function=&module->functions[f];
            const XrXirFunctionIdentity *identity=&module->declarations->functions[f];
            if(identity->module!=module->declarations->root_module || function->name_length!=strlen(names[n]) ||
                memcmp(function->name,names[n],function->name_length))continue;
            CHECK(identity->exported && ids[n+1]==UINT32_MAX);ids[n+1]=f;
        }
        CHECK(ids[n+1]!=UINT32_MAX);
    }
    XrXirCSource native={0};
    CHECK(xr_xir_compile_emit_c(lowered,"fixed_transfer",16777216,&native)==XR_XIR_OK);
    if(generated) {
        CHECK(fwrite(native.text,1,native.length,generated)==native.length);
        CHECK(fprintf(generated,"\nconst uint32_t fixed_transfer_ids[4]={%uu,%uu,%uu,%uu};\n",
            ids[0],ids[1],ids[2],ids[3])>0);
    }
    xr_xir_compile_c_source_free(&native);
    if(!generated) {
        XrXirProgram *program=NULL;
        CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK);
        fixed_transfer_run(context,program,ids);
    }
    xr_xir_compile_artifact_free(lowered);CHECK(fixed_held(context)==held);
}
