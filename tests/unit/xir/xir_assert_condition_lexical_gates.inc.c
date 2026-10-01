/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_assert_condition_lexical_gates.inc.c - Lexical declarations retain precedence
 */
static void assert_no_intrinsic(const XrXirSourceResult *result,uint32_t modules) {
    const XrXirModule *module=xr_xir_artifact_module(result->checked);
    const XrXirSourceView *view=xr_xir_source_snapshot_view(result->snapshot);
    CHECK(module->declarations->module_count==modules && view->module_count==modules);
    for (uint32_t f=0;f<module->function_count;++f)
        for (uint32_t i=0;i<module->functions[f].instruction_count;++i)
            CHECK(module->functions[f].instructions[i].op!=XR_XIR_ASSERT_CONDITION);
    for (uint32_t d=0;d<view->declaration_count;++d) CHECK(!view->declarations[d].native_identity);
}
static void assert_lexical(const char *directory,const char *path) {
    const char *sources[]={
        "const assert=fn(cond:bool){};assert(false)\n",
        "fn run(assert:fn(bool)){assert(false)};run(fn(cond:bool){})\n",
        "fn len(value:string)->bool{return true};assert(len(\"x\"))\n"};
    for (uint32_t i=0;i<2;++i) {
        XrXirSourceResult result=assert_source(directory,path,sources[i],true);
        assert_no_intrinsic(&result,1);xr_xir_source_result_free(&result);
    }
    char library[2048];CHECK(snprintf(library,sizeof(library),"%s/shadow.xr",directory)>0);
    assert_write(library,"export fn assert(cond:bool,msg:string=\"\"){}\n");
    XrXirSourceResult result=assert_source(directory,path,
        "import {assert} from \"./shadow\";assert(false)\n",true);
    assert_no_intrinsic(&result,2);xr_xir_source_result_free(&result);CHECK(!remove(library));
    result=assert_source(directory,path,sources[2],true);xr_xir_source_result_free(&result);
    result=assert_source(directory,path,"const values=[true];assert(values[0])\n",true);
    const XrXirModule *module=xr_xir_artifact_module(result.checked);
    const XrXirSourceView *view=xr_xir_source_snapshot_view(result.snapshot);
    CHECK(module->declarations->module_count==2 && view->module_count==3);
    unsigned core=0,array=0;
    for (uint32_t d=0;d<view->declaration_count;++d) {
        const XrXirSourceDeclaration *record=&view->declarations[d];
        if (record->kind==XR_XIR_SOURCE_INTRINSIC && record->native_identity==XR_CORE_BUILTIN_ASSERT) {
            CHECK(record->range.module==2);++core;
        }
        if (record->kind==XR_XIR_SOURCE_TYPE && record->native_identity==XR_NATIVE_DECLARATION_ARRAY) {
            CHECK(record->range.module==1);++array;
        }
    }
    CHECK(core==1 && array==1);
    CHECK(module->defaults && module->defaults->count==2);
    CHECK(module->declarations->functions[module->defaults->records[0].owner].module==1);
    xr_xir_source_result_free(&result);
    CHECK(!source_live && !source_bytes && !runtime_live && !runtime_bytes);
    puts("Local/parameter/import shadows and query-only Array before real Core module PASS");
}
