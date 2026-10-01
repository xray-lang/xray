/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_panics_authority.inc.c - Result roles do not confer source authority
 */
static void panics_no_intrinsic(const XrXirSourceResult *result,uint32_t modules) {
    const XrXirModule *module=xr_xir_artifact_module(result->checked);
    const XrXirSourceView *view=xr_xir_source_snapshot_view(result->snapshot);
    CHECK(view && view->module_count==modules && module->declarations->module_count==modules);
    for (uint32_t f=0;f<module->function_count;++f) {
        CHECK(!module->generics || !module->generics[f].parameter_kinds);
        for (uint32_t i=0;i<module->functions[f].instruction_count;++i)
            CHECK(module->functions[f].instructions[i].op!=XR_XIR_ASSERT_CONDITION &&
                module->functions[f].instructions[i].op!=XR_XIR_INVOKE_DISCARD);
    }
    for (uint32_t d=0;d<view->declaration_count;++d) CHECK(!view->declarations[d].native_identity);
}
static void panics_authority(const char *directory,const char *path) {
    const char *sources[]={"const assertPanics=fn(action:fn()){};assertPanics(fn(){})\n",
        "fn run(assertPanics:fn(fn())){assertPanics(fn(){})};run(fn(action:fn()){})\n"};
    for (uint32_t i=0;i<2;++i) {
        XrXirSourceResult result=panics_source(directory,path,sources[i],true);
        panics_no_intrinsic(&result,1);xr_xir_source_result_free(&result);
    }
    char library[2048];CHECK(snprintf(library,sizeof(library),"%s/ordinary.xr",directory)>0);
    panics_write(library,"export fn assertPanics<T>(action:fn()->T,msg:string=\"ordinary\"){}\n");
    XrXirSourceResult result=panics_source(directory,path,
        "import \"./ordinary\" as core;core.assertPanics<i64>(fn()->i64{return 17})\n",true);
    panics_no_intrinsic(&result,2);xr_xir_source_result_free(&result);CHECK(!remove(library));
    panics_write(library,"struct Private{value:string};export fn hidden()->Private{return Private{value:\"owned\"}}\n");
    result=panics_source(directory,path,"import \"./ordinary\" as lib;assertPanics(fn(){return lib.hidden()})\n",false);
    xr_xir_source_result_free(&result);CHECK(!remove(library));
    result=panics_source(directory,path,"fn ordinary<T>(value:T)->T{return value};ordinary<i64>(17)\n",true);
    XrXirModule *module=&result.checked->module;uint32_t function=panics_find(module,"ordinary");
    XrXirGeneric *generic=(XrXirGeneric *)&module->generics[function];uint32_t kind=XR_XIR_BINDER_RESULT_VARIABLE;
    generic->parameter_kinds=&kind;XrXirCheckedPacket packet={0};XrXirArtifact *closed=NULL;
    CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL)==XR_XIR_BAD_TYPE && !packet.bytes);
    CHECK(xr_xir_specialize(result.checked,NULL,&closed,NULL)==XR_XIR_BAD_TYPE && !closed);generic->parameter_kinds=NULL;
    XrXirSourceView *view=(XrXirSourceView *)xr_xir_source_snapshot_view(result.snapshot);
    XrXirSourceDeclaration *records=(XrXirSourceDeclaration *)view->declarations;
    for (uint32_t d=0;d<view->declaration_count;++d) if (records[d].kind==XR_XIR_SOURCE_FUNCTION) {
        records[d].kind=XR_XIR_SOURCE_INTRINSIC;records[d].native_identity=XR_CORE_BUILTIN_ASSERT_PANICS;break;
    }
    CHECK(xr_xir_checked_write(result.checked,NULL,&packet,NULL)==XR_XIR_OK);
    xr_xir_checked_packet_free(&packet);xr_xir_source_result_free(&result);
    result=panics_source(directory,path,xir_core_declaration_source,false);xr_xir_source_result_free(&result);
    CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
    puts("Lexical/namespace ordinary generic precedence; forged result role denied; query native identity grants no recipe PASS");
}
