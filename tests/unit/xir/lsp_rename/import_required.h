/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * import_required.h - Exact import binding and export selections
 *
 * KEY CONCEPT:
 *   Checked Source supplies binding identity and parser token observations.
 *   Import selectors and local names keep independent semantic roles.
 */
#ifndef LSP_IMPORT_REQUIRED_H
#define LSP_IMPORT_REQUIRED_H
#include "import_owner.h"
#include "app/lsp/xlsp_source_workspace.h"
typedef struct ImportExpectedEdit {
    unsigned line,start,end;
    const char *text;
} ImportExpectedEdit;
static void import_edit_fact(XrJsonValue *edit,ImportExpectedEdit expected) {
    CHECK(edit&&edit->type==XR_JSON_OBJECT);
    const char *text=xjson_get_string(edit,"newText");CHECK(text&&!strcmp(text,expected.text));
    XrJsonValue *range=xjson_get_object(edit,"range");CHECK(range);
    XrJsonValue *start=xjson_get_object(range,"start"),*end=xjson_get_object(range,"end");CHECK(start&&end);
    CHECK(xjson_get_int(start,"line")==expected.line&&xjson_get_int(end,"line")==expected.line);
    CHECK(xjson_get_int(start,"character")==expected.start&&xjson_get_int(end,"character")==expected.end);
}
static void import_result_fact(XrJsonValue *result,const char *uri,const char *library,unsigned mode,const char *name) {
    XrJsonValue *changes=xjson_get_array(result,"documentChanges");CHECK(changes&&changes->as.array.count==(mode==3?2:1));
    for(int d=0;d<changes->as.array.count;++d) {
        XrJsonValue *document=xjson_array_get(changes,d),*identity=xjson_get_object(document,"textDocument");
        XrJsonValue *edits=xjson_get_array(document,"edits");CHECK(identity&&edits);
        CHECK(!strcmp(xjson_get_string(identity,"uri"),mode==3&&d==1?library:uri));
        CHECK(xjson_get(identity,"version")->is_integer&&xjson_get_int(identity,"version")==(mode==3&&d==1?9:7));
        CHECK(edits->as.array.count==(mode==3?1:2));
        if(mode==3)import_edit_fact(xjson_array_get(edits,0),d==0?
            (ImportExpectedEdit){0,8,14,"changed as target"}:(ImportExpectedEdit){0,10,16,name});
        else {
            import_edit_fact(xjson_array_get(edits,0),(ImportExpectedEdit){1,21,mode==1||mode==4?24:mode==2?27:26,name});
            ImportExpectedEdit declared=mode==1?(ImportExpectedEdit){0,18,21,name}:
                mode==2?(ImportExpectedEdit){0,8,14,"target as changed"}:mode==4?(ImportExpectedEdit){0,14,14," as changed"}:
                mode==5?(ImportExpectedEdit){0,27,32,name}:(ImportExpectedEdit){0,18,23,name};
            import_edit_fact(xjson_array_get(edits,1),declared);
        }
    }
}
static const char *import_program(unsigned mode) {
    static const char *const programs[]={
        "import {target as alias} from \"./lib\"\nfn run()->i64{return alias(1)}\n",
        "import \"./lib\" as lib\nfn run()->i64{return lib.target(1)}\n",
        "import {target} from \"./lib\"\nfn run()->i64{return target(1)}\n",
        "import {target} from \"./lib\"\nfn run()->i64{return target(1)}\n",
        "import \"./lib\"\nfn run()->i64{return lib.target(1)}\n",
        "/* \xf0\x9f\x98\x80 */ import {target as alias} from \"./lib\"\r\nfn run()->i64{return alias(1)}\r\n"
    };
    CHECK(mode<6);return programs[mode];
}
static const char import_child[]="export fn target(value:i64)->i64{return value}\n";
static void import_semantic(const char *uri,const char *library,const char *program,unsigned mode) {
    XrCompileResources *r=sdk_ledger(&limits);XrXirCompileContext context={r,xr_xir_compile_default_limits()};
    XlspSourceDocument documents[]={{uri,program,strlen(uri),strlen(program),7},
        {library,import_child,strlen(library),sizeof(import_child)-1,9}};
    XlspSourceWorkspace *workspace=NULL;XlspSourceWorkspaceFailure failure={0};unsigned stage=0;
    XrXirStatus status=xlsp_source_workspace_build(&context,documents,2,&workspace,&stage,&failure);
    XlspSourceTokens *tokens=NULL;
    if(status==XR_XIR_OK)status=xlsp_source_workspace_tokens(workspace,uri,strlen(uri),NULL,&tokens);
    if(status!=XR_XIR_OK)fprintf(stderr,"Import semantic mode=%u status=%u stage=%u %s\n",mode,(unsigned)status,stage,failure.diagnostic.message);
    CHECK(status==XR_XIR_OK&&tokens);
    XrXirSourceDeclarationKind direct=XR_XIR_SOURCE_INTRINSIC;
    XrLspPosition named={0,mode==2||mode==3?8u:mode==4?14u:mode==5?27u:18u};
    status=xlsp_source_workspace_declaration_kind(workspace,uri,strlen(uri),named,&direct);
    if(mode==4)CHECK(status==XR_XIR_UNRESOLVED&&direct==XR_XIR_SOURCE_INTRINSIC);
    else CHECK(status==XR_XIR_OK&&direct==(mode==1?XR_XIR_SOURCE_MODULE:XR_XIR_SOURCE_IMPORT));
    direct=XR_XIR_SOURCE_INTRINSIC;
    CHECK(xlsp_source_workspace_declaration_kind(workspace,uri,strlen(uri),(XrLspPosition){1,22},&direct)==XR_XIR_UNRESOLVED);
    CHECK(direct==XR_XIR_SOURCE_INTRINSIC);
    size_t count=0;const XlspSourceToken *items=xlsp_source_tokens_items(tokens,&count);CHECK(count);
    bool qualifier=false;
    for(size_t i=0;i<count;++i) {
        CHECK(items[i].length&&items[i].type!=16);
        if(items[i].line==1&&items[i].column==21) {
            CHECK(items[i].length==(mode==1||mode==4?3u:mode==2||mode==3?6u:5u));
            CHECK(items[i].type==(mode==1||mode==4?0u:12u));qualifier=true;
        }
    }
    CHECK(qualifier);xlsp_source_workspace_free(workspace);xlsp_source_workspace_failure_free(&failure);xr_compile_resources_release(r);
    CHECK(xlsp_source_tokens_items(tokens,&count)&&count);xlsp_source_tokens_free(tokens);CHECK(!runtime_live&&!runtime_bytes);
}
static int import_required(const char *uri,const char *library) {
    import_owner();
    unsigned passed=0;
    for(unsigned mode=0;mode<6;++mode){
        XrCompileResources *r=sdk_ledger(&limits);
        XrXirCompileContext context={r,xr_xir_compile_default_limits()};
        const char *program=import_program(mode);
        XlspSourceDocument documents[]={{uri,program,strlen(uri),strlen(program),7},
            {library,import_child,strlen(library),sizeof(import_child)-1,9}};
        XlspSourceBindings *owner=NULL;XrXirStatus status=xlsp_source_bindings_build(&context,documents,2,&owner);
        CHECK(status==XR_XIR_OK&&owner);
        const XrXirSourceView *view=xlsp_source_bindings_view(owner,0);
        const XrXirSourceSyntaxView *syntax=xlsp_source_bindings_syntax(owner,0);
        for(uint32_t i=0;i<view->declaration_count;++i){
            const XrXirSourceDeclaration *decl=&view->declarations[i];
            if(decl->kind!=XR_XIR_SOURCE_IMPORT&&decl->kind!=XR_XIR_SOURCE_MODULE)continue;
            XrXirSourceRange span=syntax->declarations[i].name;
            fprintf(stderr,"Import fact mode=%u id=%u kind=%u name=%s role=%u span=%d:%d..%d:%d\n",
                mode,decl->id,(unsigned)decl->kind,decl->name,(unsigned)syntax->declarations[i].role,
                span.line,span.column,span.end_line,span.end_column);
        }
        xlsp_source_bindings_free(owner);
        const char *name=mode==5?"changed\xf0\x9f\x98\x80":"changed";
        XlspSourceRenameInput input={mode==3?1u:0u,mode==3?(XrLspPosition){0,12}:(XrLspPosition){1,22},name,strlen(name),false,NULL};
        XrJsonValue *result=NULL;status=xlsp_source_rename_json(&context,documents,2,&input,&result);
        bool accepted=status==XR_XIR_OK&&result;
        if(accepted){import_result_fact(result,uri,library,mode,name);++passed;}
        if(mode==3) {
            xjson_free(result);result=NULL;input.entry=0;input.position=(XrLspPosition){0,10};
            CHECK(xlsp_source_rename_json(&context,documents,2,&input,&result)==XR_XIR_OK&&result);
            import_result_fact(result,uri,library,3,name);
        }
        fprintf(stderr,"Import required mode=%u status=%u output=%s accepted=%u\n",mode,(unsigned)status,result?"JSON":"NULL",accepted?1u:0u);
        xjson_free(result);xr_compile_resources_release(r);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
        import_semantic(uri,library,program,mode);
    }
    fprintf(stderr,"Import required passed=%u/6 compiler_and_JSON_physical0/0\n",passed);CHECK(passed==6);return 0;
}
#endif // LSP_IMPORT_REQUIRED_H
