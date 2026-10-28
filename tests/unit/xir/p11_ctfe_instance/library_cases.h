/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * library_cases.h - Source-free canonical calls receive later substitutions
 */
#include "xir/xxir_library_catalog.h"
static const char *library_comptime_text(const char *kind) {
    if (!strcmp(kind,"closed")) return "export fn make()->Array<i8>{return comptime []}\n";
    if (!strcmp(kind,"closed-method")) return "export struct Factory{static make()->Array<i8>{return comptime []}}\n";
    if (!strcmp(kind,"text")) return "fn zero<T>()->string{return \"a\\0b\"}\nexport fn make<T>()->string{return comptime zero<T>()}\n";

    if (!strcmp(kind,"private")) return "export struct Box<T>{private value:T?;static empty()->Box<T>{return Box<T>{value:null}};size()->i64{const v=this.value;return 42}}\n"
        "export fn make<T>()->Box<T>{return comptime Box<T>.empty()}\n";
    if (!strcmp(kind,"multi")) return "export fn make<T,U>()->(T?,Array<U>){return comptime (null,[])}\n";
    if (!strcmp(kind,"method")) return "export struct Factory{static make<T>()->Array<T>{return comptime []}}\n";
    if (!strcmp(kind,"nested")) return "export fn make<T>()->i64{return comptime {const a:Array<T>=comptime [];len(a)+42}}\n";
    if (!strcmp(kind,"mixed")) return "export fn make<T>()->i64{const n:i64=comptime 42;const a:Array<T>=comptime [];return n+len(a)}\n";
    if (!strcmp(kind,"builtin")) return "fn zero<T>()->i64{return 42}\nexport fn make<T>()->i64{return comptime zero<T>()}\n";
    if (!strcmp(kind,"none")) return "export fn make<T>()->i64{return 42}\n";
    return "export fn make<T>()->Array<T>{return comptime []}\n";
}
static const char *library_comptime_consumer(const char *kind) {
    if (!strcmp(kind,"closed")) return "import {make} from \"./ctfe-library\"\nexport fn run()->i64{const a=make();const b=make();return len(a)+len(b)+42}\n";
    if (!strcmp(kind,"closed-method")) return "import {Factory} from \"./ctfe-library\"\nexport fn run()->i64{const a=Factory.make();const b=Factory.make();return len(a)+len(b)+42}\n";
    if (!strcmp(kind,"text")) return "import {make} from \"./ctfe-library\"\nexport fn run()->i64{const a=make<i8>();const b=make<string>();return len(a)+len(b)+36}\n";

    if (!strcmp(kind,"local")) return "import {make} from \"./ctfe-library\"\nfn local<T>()->Array<T>{return comptime []}\n"
        "export fn run()->i64{const a=make<i8>();const b=make<string>();const c=local<i8>();const d=local<string>();return len(a)+len(b)+len(c)+len(d)+42}\n";
    if (!strcmp(kind,"fresh")) return "import {make} from \"./ctfe-library\"\nstruct Later{private value:i64}\n"
        "export fn run()->i64{const a=make<Later>();const b=make<Array<Later>>();return len(a)+len(b)+42}\n";
    if (!strcmp(kind,"known")) return "import {make} from \"./ctfe-library\"\nexport fn run()->i64{return make<i8>()+make<string>()-99}\n";
    if (!strcmp(kind,"dual")) return "import {make as left} from \"./ctfe-library\"\nimport {make as right} from \"./ctfe-second\"\n"
        "export fn run()->i64{const a=left<i8>();const b=left<string>();const c=right<i8>();const d=right<string>();return len(a)+len(b)+len(c)+len(d)+42}\n";
    if (!strcmp(kind,"private")) return "import {make} from \"./ctfe-library\"\nexport fn run()->i64{return make<i8>().size()+make<string>().size()-42}\n";
    if (!strcmp(kind,"multi")) return "import {make} from \"./ctfe-library\"\nexport fn run()->i64{const a=make<i8,string>();const b=make<string,i8>();return 42}\n";
    if (!strcmp(kind,"method")) return "import {Factory} from \"./ctfe-library\"\nexport fn run()->i64{const a=Factory.make<i8>();const b=Factory.make<string>();return len(a)+len(b)+42}\n";
    if (!strcmp(kind,"nested") || !strcmp(kind,"mixed") || !strcmp(kind,"builtin") || !strcmp(kind,"none"))
        return "import {make} from \"./ctfe-library\"\nexport fn run()->i64{return make<i8>()+make<string>()-42}\n";
    return "import {make} from \"./ctfe-library\"\nexport fn run()->i64{const a=make<i8>();const b=make<string>();return len(a)+len(b)+42}\n";
}
static XrXirStatus library_comptime_named_source(XrXirCompileContext *context,const char *name,const char *text,bool library,
    const XrXirLibraryCatalog *catalog,XrXirSourceResult *output) {
    char path[1024];
    CHECK(snprintf(path,sizeof(path),"%s/%s",MATERIALIZATION_ROOT,name)>0);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,MATERIALIZATION_ROOT};
    XrModuleOverlayInput overlay={authority,name,path,text,strlen(text)};
    XrCompilerSession *session=NULL;XrCompileResourceStatus created=xr_compile_session_new(context->resources,&session);
    if (created!=XR_COMPILE_RESOURCE_OK) return p11_resource_status(created);
    XrXirSourceRequest request={session,path,&authority,context,NULL,NULL,library?XR_XIR_LIBRARY:XR_XIR_PROGRAM,catalog};
    XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check_overlay(&request,&overlay,1,output,&diagnostic,NULL);
    xr_compile_session_free(session);
    if (status!=XR_XIR_OK && status!=XR_XIR_BUDGET && status!=XR_XIR_OUT_OF_MEMORY)
        fprintf(stderr,"Library CTFE %s status%u %d:%d %s\n",name,status,diagnostic.line,diagnostic.column,diagnostic.message);
    return status;
}
static XrXirStatus library_comptime_source(XrXirCompileContext *context,const char *text,bool library,
    const XrXirLibraryCatalog *catalog,XrXirSourceResult *output) {
    return library_comptime_named_source(context,library?"ctfe-library.xr":"ctfe-root.xr",text,library,catalog,output);
}
static XrXirStatus library_comptime_known(XrXirCompileContext *context,XrXirArtifact **output) {
    const char *text="export fn make<T>()->i64{return comptime {const a:Array<T>=[];len(a)+42}}\n"
        "export fn seed()->i64{return make<i8>()}\n";
    XrXirSourceResult source={0};XrXirStatus status=library_comptime_named_source(context,"ctfe-library.xr",text,false,NULL,&source);
    XrXirFunction *functions=NULL;XrXirInstruction *ops=NULL;
    if (status==XR_XIR_OK) {
        CHECK(source.checked->construction->comptime.binding_count==1);
        uint32_t helper=source.checked->construction->comptime.sites[0].helper;
        functions=xir_compile_copy(context,source.checked->module.functions,(size_t)source.checked->module.function_count*sizeof(*functions),&status);
        if (status==XR_XIR_OK) ops=xir_compile_copy(context,functions[helper].instructions,(size_t)functions[helper].instruction_count*sizeof(*ops),&status);
        if (status==XR_XIR_OK) {
            bool changed=false;functions[helper].instructions=ops;
            for (uint32_t i=0;i<functions[helper].instruction_count;++i) if (ops[i].op==XR_XIR_CONST_INT && ops[i].immediate==42) {
                ops[i].immediate=99;changed=true;
            }
            CHECK(changed);XrXirDeclarations declarations=*source.checked->module.declarations;
            declarations.root_module=UINT32_MAX;declarations.entry_function=UINT32_MAX;
            XrXirModule built=source.checked->module;built.stage=XR_XIR_BUILT;built.linkage_kind=XR_XIR_LIBRARY;
            built.functions=functions;built.declarations=&declarations;
            status=xr_xir_compile_check_v2(context,&built,source.checked->construction,output,NULL);
        }
    }
    xr_compile_resources_free(ops);xr_compile_resources_free(functions);xr_xir_compile_source_result_free(&source);return status;
}
#include "library_foreign.h"
static XrXirStatus library_comptime_operation(XrXirCompileContext *context,const char *kind,XrXirArtifact **output) {
    if (!strcmp(kind,"foreign")) return library_comptime_foreign_operation(context,output);
    XrXirSourceResult source={0};XrXirCheckedPacket packet={0},second={0};XrXirLibraryCatalog *catalog=NULL;
    XrXirStatus status=!strcmp(kind,"known")?library_comptime_known(context,&source.checked):
        library_comptime_source(context,library_comptime_text(kind),true,NULL,&source);
    if (status==XR_XIR_OK) {
        const XrXirComptimeTable *table=&source.checked->construction->comptime;
        CHECK(table->site_count==(!strcmp(kind,"none") || !strcmp(kind,"closed") || !strcmp(kind,"closed-method")?0u:!strcmp(kind,"nested")?2u:1u) && table->binding_count==(!strcmp(kind,"known")?1u:0u));
        if (source.snapshot) CHECK(xr_xir_compile_construction_comptime(xr_xir_compile_source_snapshot_construction(source.snapshot))->site_count==table->site_count);
        status=xr_xir_compile_checked_write(source.checked,&packet,NULL);
    }
    xr_xir_compile_source_result_free(&source);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,MATERIALIZATION_ROOT};
    XrXirLibraryModuleInput module={authority,"ctfe-library.xr"};
    XrXirLibraryModuleInput extra={authority,"ctfe-second.xr"};
    if (status==XR_XIR_OK && !strcmp(kind,"dual")) {
        status=library_comptime_named_source(context,"ctfe-second.xr",library_comptime_text("array"),true,NULL,&source);
        if (status==XR_XIR_OK) status=xr_xir_compile_checked_write(source.checked,&second,NULL);
        xr_xir_compile_source_result_free(&source);
    }
    XrXirLibraryInput inputs[2]={{packet.bytes,packet.length,{0},&module,1},{second.bytes,second.length,{0},&extra,1}};
    if (status==XR_XIR_OK) {
        xr_sha256(packet.bytes,packet.length,inputs[0].sha256);
        if (second.bytes) xr_sha256(second.bytes,second.length,inputs[1].sha256);
        status=xr_xir_compile_library_catalog_new_v2(context,inputs,second.bytes?2:1,&catalog);
    }
    if (second.bytes) memset(second.bytes,0xa5,second.length);xr_xir_compile_checked_packet_free(&second);
    if (packet.bytes) memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    if (status==XR_XIR_OK) status=library_comptime_source(context,library_comptime_consumer(kind),false,catalog,&source);
    xr_xir_compile_library_catalog_free(catalog);
    if (status==XR_XIR_OK) { *output=source.checked;source.checked=NULL; }
    xr_xir_compile_source_result_free(&source);return status;
}
static void library_comptime_normal(const char *kind) {
    XrXirCompileContext context;context_new(&context);XrXirArtifact *source=NULL,*received=NULL,*closed=NULL,*lowered=NULL;
    XrXirStatus status=library_comptime_operation(&context,kind,&source);
    if (status!=XR_XIR_OK) fprintf(stderr,"Library %s status%u\n",kind,status);
    CHECK(status==XR_XIR_OK && source);
    XirComptimePending *pending=NULL;CHECK(xr_xir_compile_comptime_pending(source,&pending,NULL)==XR_XIR_OK && !pending);
    uint32_t sites=source->construction->comptime.site_count,bindings=source->construction->comptime.binding_count;
    bool doubled=!strcmp(kind,"local") || !strcmp(kind,"dual");
    bool absent=!strcmp(kind,"none") || !strcmp(kind,"closed") || !strcmp(kind,"closed-method");
    CHECK(sites==(absent?0u:!strcmp(kind,"nested") || doubled?2u:1u) &&
        bindings==(absent?0u:doubled?4u:2u));
    XrXirCheckedPacket packet={0};CHECK(xr_xir_compile_checked_write(source,&packet,NULL)==XR_XIR_OK);
    xr_xir_compile_artifact_free(source);CHECK(xr_xir_compile_checked_read(&context,packet.bytes,packet.length,&received,NULL)==XR_XIR_OK);
    memset(packet.bytes,0xa5,packet.length);xr_xir_compile_checked_packet_free(&packet);
    CHECK(xr_xir_compile_specialize(received,&closed,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(received);
    const XrXirTarget target={XR_XIR_ARCH_X86_64,XR_XIR_VALUE_ABI_VERSION};
    CHECK(xr_xir_compile_lower(closed,&target, NULL,&lowered,NULL)==XR_XIR_OK);xr_xir_compile_artifact_free(closed);
    uint32_t run=named(&lowered->module,"run");XrXirProgram *program=NULL;
    CHECK(xr_xir_compile_vm_program_take(&lowered,&program)==XR_XIR_OK && !lowered);xr_compile_resources_release(context.resources);
    for (unsigned i=0;i<2;++i) {
        XrXirInstanceConfig config;CHECK(xr_xir_instance_config_init(&config,sizeof(config))==XR_XIR_CALL_READY);
        XrXirInstance *instance=NULL;CHECK(xr_xir_instance_new(program,&config,&instance)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_start(instance,run,NULL,0)==XR_XIR_CALL_READY);
        CHECK(xr_xir_instance_poll_bounded(instance,UINT64_MAX).outcome.status==XR_XIR_CALL_RETURNED);
        XrXirValue value={0};CHECK(xr_xir_instance_take_result(instance,&value)==XR_XIR_CALL_RETURNED && value.type==XR_XIR_I64 && value.payload==42);
        xr_xir_value_drop(&value);CHECK(xr_xir_instance_stop(instance)==XR_XIR_CALL_READY && xr_xir_instance_free(instance)==XR_XIR_CALL_READY);
    }
    size_t attempts=source_fixture_compile_attempts;source_fixture_compile_fail_at=attempts;
    xr_xir_compile_program_drop(program);CHECK(source_fixture_compile_attempts==attempts);source_fixture_compile_fail_at=SIZE_MAX;
    CHECK(!source_fixture_compile_live && !source_fixture_compile_bytes);
    fprintf(stderr,"Library %s canonical sites%u later keys%u, no library AST/catalog/packet, two real runtime Instances42 physical0\n",kind,sites,bindings);
}
