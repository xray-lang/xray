/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_source_core_syntax_cases.h - Generated core declarations retain owned syntax
 *
 * KEY CONCEPT:
 *   Earlier native declarations cannot shift the syntax identity of core facts.
 */
#ifndef XIR_SOURCE_CORE_SYNTAX_CASES_H
#define XIR_SOURCE_CORE_SYNTAX_CASES_H
static void source_core_syntax_facts(XrXirSourceRequest *request) {
    XrCompileResourceStats before=stage_stats(request->context);
    size_t blocks=stage_physical_count,bytes=stage_physical_bytes;
    write_source(request->entry_path,
        "fn equal<T:Equal>(a:T,b:T){assertEqual(a,b);}\n"
        "const counter=Atomic(1)\n"
        "equal(1,1)\nassert(true)\nassertPanics(fn()->i64{return 1/0;})\n");
    XrCompilerSession *session=NULL;
    CHECK(xr_compile_session_new(request->context->resources,&session)==XR_COMPILER_SESSION_OK);
    XrXirSourceRequest local=*request;local.session=session;
    XrXirSourceResult result={0};XrXirSourceDiagnostic diagnostic={0};
    XrXirStatus status=xr_xir_compile_source_check(&local,&result,&diagnostic,NULL);
    if (status!=XR_XIR_OK) fprintf(stderr,"core syntax %u at %d:%d: %s\n",status,diagnostic.line,diagnostic.column,diagnostic.message);
    CHECK(status==XR_XIR_OK && result.checked && result.snapshot);
    xr_xir_compile_artifact_free(result.checked);result.checked=NULL;
    xr_compile_session_free(session);
    write_source(request->entry_path,"const replaced=0\n");
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(result.snapshot);
    CHECK(view && syntax && syntax->declaration_count==view->declaration_count);
    CHECK(syntax->reference_count==view->reference_count);
    const char *names[]={"assert","assertPanics","assertEqual"};
    const int lines[]={3,4,6},ends[]={17,23,22};
    uint32_t module=UINT32_MAX;
    for (uint32_t i=0;i<3;++i) {
        const XrXirSourceDeclaration *decl=declaration(view,names[i],0);
        CHECK(decl && decl->kind==XR_XIR_SOURCE_INTRINSIC && decl->native_identity);
        const XrXirSourceDeclarationSyntax *fact=&syntax->declarations[decl->id-1];
        CHECK(fact->role==XR_XIR_SOURCE_SYNTAX_NAME && !fact->flags);
        CHECK(fact->name.module==decl->range.module && fact->name.module<view->module_count);
        CHECK(fact->name.line==lines[i] && fact->name.column==11);
        CHECK(fact->name.end_line==lines[i] && fact->name.end_column==ends[i]);
        if (!i) module=fact->name.module;
        else CHECK(fact->name.module==module);
    }
    CHECK(module<view->module_count && !view->modules[module].path);
    CHECK(strstr(view->modules[module].identity,"xray-core-assertions-v1"));
    for (uint32_t i=0;i<view->declaration_count;++i)
        CHECK(syntax->declarations[i].name.module==view->declarations[i].range.module);
    xr_xir_compile_source_result_free(&result);
    XrCompileResourceStats after=stage_stats(request->context);
    CHECK(after.live_bytes==before.live_bytes);
    CHECK(stage_physical_count==blocks && stage_physical_bytes==bytes);
}
#endif // XIR_SOURCE_CORE_SYNTAX_CASES_H
