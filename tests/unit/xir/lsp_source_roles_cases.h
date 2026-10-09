#include "xir/xxir_source_query_internal.h"
static const char roles_source[]=
    "/* \xf0\x9f\x98\x80 */ struct S {\n"
    " value:i64\n"
    " ref bump(x:ref i64)->i64 { this.value += x; return this.value }\n"
    " static pick()->i64 { return 1 }\n"
    "}\n"
    "fn outer()->i64 { const f=fn(y:i64)->i64{return y}; return f(2) }\n"
    "fn names(ref:i64,move:i64)->i64{return ref+move}\n"
    "fn change(x:ref i64)->i64{return x}\n"
    "fn use()->i64{var n=3;return change(ref n)}\n";
static void roles_token(const XlspSourceTokens *owner,uint32_t row,uint32_t column,uint32_t length,uint32_t type,uint32_t modifiers) {
    size_t count=0;const XlspSourceToken *tokens=xlsp_source_tokens_items(owner,&count);unsigned found=0;
    for(size_t i=0;i<count;++i)if(tokens[i].line==row&&tokens[i].column==column) {
        CHECK(tokens[i].length==length&&tokens[i].type==type&&tokens[i].modifiers==modifiers);++found;
    }
    CHECK(found==1);
}
static void roles_tokens(const XlspSourceTokens *tokens) {
    /* Fixed UTF16 locations, from the literal alone. No emitted token range
     * supplies these expected positions or role/modifier classifications. */
    roles_token(tokens,0,16,1,5,3);
    roles_token(tokens,1,1,5,9,1);
    roles_token(tokens,2,1,3,16,0);roles_token(tokens,2,5,4,13,3);
    roles_token(tokens,2,10,1,7,1);roles_token(tokens,2,12,3,16,0);
    roles_token(tokens,2,33,5,9,64);roles_token(tokens,2,57,5,9,0);
    roles_token(tokens,3,8,4,13,11);
    roles_token(tokens,5,3,5,12,3);roles_token(tokens,5,24,1,8,5);
    roles_token(tokens,5,29,1,7,5);roles_token(tokens,5,48,1,7,4);
    roles_token(tokens,6,9,3,7,5);roles_token(tokens,6,17,4,7,5);
    roles_token(tokens,6,39,3,7,4);roles_token(tokens,6,43,4,7,4);
    roles_token(tokens,7,12,3,16,0);roles_token(tokens,8,36,3,16,0);
    size_t count=0;const XlspSourceToken *items=xlsp_source_tokens_items(tokens,&count);unsigned markers=0;
    for(size_t i=0;i<count;++i) {
        markers+=items[i].type==16;
        CHECK(!(items[i].line==5&&items[i].column<=26&&items[i].column+items[i].length>26));
    }
    CHECK(markers==4); /* Named ref/move parameters are not modifiers. */
}
static XrXirStatus roles_once(const char *root,XrCompileResourceLimits limits,XrCompileResourceStats *stats) {
    XrXirCompileContext context={0};XrCompilerSession *session=NULL;XlspSourceSnapshot *snapshot=NULL;XlspSourceTokens *tokens=NULL;
    XrCompileResourceStatus created=xr_compile_resources_new(&limits,&context.resources);
    if(created!=XR_COMPILE_RESOURCE_OK)return created==XR_COMPILE_RESOURCE_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    context.limits=xr_xir_compile_default_limits();XrCompilerSessionStatus opened=xr_compile_session_new(context.resources,&session);
    XrXirStatus status=opened==XR_COMPILER_SESSION_OK?XR_XIR_OK:opened==XR_COMPILER_SESSION_BUDGET?XR_XIR_BUDGET:XR_XIR_OUT_OF_MEMORY;
    char uri[XR_TEST_PATH_MAX],text[sizeof(roles_source)];lsp_uri(uri,sizeof(uri),root,"roles.xr");memcpy(text,roles_source,sizeof(text));
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    XlspSourceDocument document={uri,text,strlen(uri),sizeof(text)-1,3};
    if(status==XR_XIR_OK)status=xlsp_source_snapshot_build(&request,&document,1,0,&snapshot,NULL,NULL);
    xr_compile_session_free(session);memset(text,'!',sizeof(text));
    XlspSourceHover hover={0};
    if(status==XR_XIR_OK)status=xlsp_source_hover(snapshot,uri,strlen(uri),(XrLspPosition){2,6},&hover);
    if(status==XR_XIR_OK)CHECK(!strcmp(hover.text,"ref fn S.bump(ref x: i64) -> i64"));
    xlsp_source_hover_free(&hover);
    if(status==XR_XIR_OK)status=xlsp_source_hover(snapshot,uri,strlen(uri),(XrLspPosition){3,9},&hover);
    if(status==XR_XIR_OK)CHECK(!strcmp(hover.text,"static fn S.pick() -> i64"));
    xlsp_source_hover_free(&hover);
    if(status==XR_XIR_OK)status=xlsp_source_hover(snapshot,uri,strlen(uri),(XrLspPosition){5,30},&hover);
    if(status==XR_XIR_OK)CHECK(!strcmp(hover.text,"parameter y: i64"));
    xlsp_source_hover_free(&hover);
    XlspSourceLocation definition={0};XlspSourceLocations references={0};
    if(status==XR_XIR_OK)status=xlsp_source_definition(snapshot,uri,strlen(uri),(XrLspPosition){2,34},&definition);
    if(status==XR_XIR_OK)CHECK(definition.range.start.line==1&&definition.range.start.character==1&&definition.range.end.character==6);
    if(status==XR_XIR_OK)status=xlsp_source_references(snapshot,uri,strlen(uri),(XrLspPosition){2,34},false,&references);
    if(status==XR_XIR_OK) {
        CHECK(references.count==2);
        CHECK(references.items[0].range.start.line==2&&references.items[0].range.start.character==33&&references.items[0].range.end.character==38);
        CHECK(references.items[1].range.start.line==2&&references.items[1].range.start.character==57&&references.items[1].range.end.character==62);
    }
    xlsp_source_locations_free(&references);
    if(status==XR_XIR_OK) {
        XlspSourceLocation prior=definition;
        XrXirStatus probe=xlsp_source_definition(snapshot,uri,strlen(uri),(XrLspPosition){2,32},&definition);
        if(probe!=XR_XIR_UNRESOLVED){CHECK(probe==XR_XIR_BUDGET||probe==XR_XIR_OUT_OF_MEMORY);status=probe;}
        CHECK(definition.uri==prior.uri&&definition.range.start.line==prior.range.start.line&&definition.range.start.character==prior.range.start.character&&definition.range.end.character==prior.range.end.character);
        if(status==XR_XIR_OK) {
            probe=xlsp_source_hover(snapshot,uri,strlen(uri),(XrLspPosition){5,26},&hover);
            if(probe!=XR_XIR_UNRESOLVED){CHECK(probe==XR_XIR_BUDGET||probe==XR_XIR_OUT_OF_MEMORY);status=probe;}
        }
        CHECK(!hover.text&&!hover.length);
    }
    XlspSourceHover escaped={0};
    if(status==XR_XIR_OK)status=xlsp_source_hover(snapshot,uri,strlen(uri),(XrLspPosition){2,6},&escaped);
    if(status==XR_XIR_OK) {
        char *original=escaped.text;size_t original_length=escaped.length;
        CHECK(xlsp_source_hover(snapshot,uri,strlen(uri),(XrLspPosition){3,9},&escaped)==XR_XIR_BAD_STRUCTURE);
        CHECK(escaped.text==original&&escaped.length==original_length);
    }
    if(status==XR_XIR_OK)status=xlsp_source_semantic_tokens(snapshot,uri,strlen(uri),&tokens);
    if(status==XR_XIR_OK)roles_tokens(tokens);else CHECK(!tokens);
    xlsp_source_snapshot_free(snapshot);
    if(status==XR_XIR_OK)CHECK(!strcmp(escaped.text,"ref fn S.bump(ref x: i64) -> i64"));
    xlsp_source_hover_free(&escaped);
    if(status==XR_XIR_OK)roles_tokens(tokens); /* Owned tokens after every producer dies. */
    xlsp_source_tokens_free(tokens);
    if(stats)CHECK(xr_compile_resources_stats(context.resources,stats)==XR_COMPILE_RESOURCE_OK);
    xr_compile_resources_release(context.resources);lsp_zero();return status;
}
static void roles_owner_cases(const char *root) {
    XrCompileResourceLimits limits=lsp_limits();XrXirCompileContext context={0};XrCompilerSession *session=NULL;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    char entry[XR_TEST_PATH_MAX];int entry_length=snprintf(entry,sizeof(entry),"%s/roles.xr",root);
    CHECK(entry_length>0&&(size_t)entry_length<sizeof(entry));request.entry_path=entry;
    XrXirSourceResult result={0};char text[sizeof(roles_source)];memcpy(text,roles_source,sizeof(text));
    CHECK(xr_xir_compile_source_check_text(&request,&(XrXirSourceText){"roles.xr",text,sizeof(text)-1},&result,NULL,NULL)==XR_XIR_OK);
    xr_compile_session_free(session);session=NULL;memset(text,'?',sizeof(text));
    const XrXirSourceView *view=xr_xir_compile_source_snapshot_view(result.snapshot);
    const XrXirSourceSyntaxView *syntax=xr_xir_compile_source_snapshot_syntax(result.snapshot);
    CHECK(syntax&&syntax->declaration_count==view->declaration_count&&syntax->reference_count==view->reference_count&&syntax->marker_count==4);
    unsigned methods=0,closures=0,receivers=0;uint32_t method=UINT32_MAX;
    for(uint32_t i=0;i<syntax->declaration_count;++i) {
        const XrXirSourceDeclarationSyntax *d=&syntax->declarations[i];
        if(d->role==XR_XIR_SOURCE_SYNTAX_METHOD) {
            ++methods;if(!strcmp(view->declarations[i].name,"bump")){method=i;CHECK(!d->flags&&d->name.line==3&&d->name.column==6&&d->name.end_column==10);}
            else CHECK(!strcmp(view->declarations[i].name,"pick")&&d->flags==XR_XIR_SOURCE_SYNTAX_STATIC);
        }
        if(d->role==XR_XIR_SOURCE_SYNTAX_CLOSURE){++closures;CHECK(!d->name.line&&!d->name.column);}
        if(d->role==XR_XIR_SOURCE_SYNTAX_RECEIVER){++receivers;CHECK(!d->name.line&&!d->name.column);}
    }
    CHECK(methods==2&&closures==1&&receivers==1&&method!=UINT32_MAX);
    XrXirSourceSnapshot *clone=NULL;
    CHECK(xr_xir_compile_source_snapshot_copy_v2(&context,view,xr_xir_compile_source_snapshot_construction(result.snapshot),&clone)==XR_XIR_OK);
    CHECK(!xr_xir_compile_source_snapshot_syntax(clone));
    XrXirSourceSyntaxView bad=*syntax;--bad.declaration_count;
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(clone,&bad)==XR_XIR_BAD_STRUCTURE&&!xr_xir_compile_source_snapshot_syntax(clone));
    XrXirSourceDeclarationSyntax *changed=NULL;
    CHECK(xr_compile_resources_alloc(context.resources,(size_t)syntax->declaration_count*sizeof(*changed),(void **)&changed)==XR_COMPILE_RESOURCE_OK);
    memcpy(changed,syntax->declarations,(size_t)syntax->declaration_count*sizeof(*changed));
    bad=*syntax;bad.declarations=changed;changed[method].flags=UINT32_MAX;
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(clone,&bad)==XR_XIR_BAD_STRUCTURE&&!xr_xir_compile_source_snapshot_syntax(clone));
    changed[method]=syntax->declarations[method];changed[method].role=XR_XIR_SOURCE_SYNTAX_RECEIVER;
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(clone,&bad)==XR_XIR_BAD_STRUCTURE&&!xr_xir_compile_source_snapshot_syntax(clone));
    changed[method]=syntax->declarations[method];changed[method].name.module=view->module_count;
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(clone,&bad)==XR_XIR_BAD_STRUCTURE&&!xr_xir_compile_source_snapshot_syntax(clone));
    xr_compile_resources_free(changed);
    XrXirSourceMarker changed_markers[4];memcpy(changed_markers,syntax->markers,sizeof(changed_markers));
    bad=*syntax;bad.markers=changed_markers;changed_markers[0].role=(XrXirSourceMarkerRole)99;
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(clone,&bad)==XR_XIR_BAD_STRUCTURE&&!xr_xir_compile_source_snapshot_syntax(clone));
    changed_markers[0]=syntax->markers[0];++changed_markers[0].range.end_column;
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(clone,&bad)==XR_XIR_BAD_STRUCTURE&&!xr_xir_compile_source_snapshot_syntax(clone));
    size_t baseline_live=source_fixture_compile_live,baseline_bytes=source_fixture_compile_bytes;
    for(size_t fail=0;fail<3;++fail) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=fail;source_fixture_compile_injected=false;
        CHECK(xr_xir_compile_source_snapshot_syntax_copy(clone,syntax)==XR_XIR_OUT_OF_MEMORY&&source_fixture_compile_injected);
        CHECK(!xr_xir_compile_source_snapshot_syntax(clone)&&source_fixture_compile_live==baseline_live&&source_fixture_compile_bytes==baseline_bytes);
    }
    source_fixture_compile_fail_at=SIZE_MAX;source_fixture_compile_injected=false;
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(clone,syntax)==XR_XIR_OK);
    CHECK(xr_xir_compile_source_snapshot_syntax_copy(clone,syntax)==XR_XIR_BAD_STRUCTURE);
    xr_xir_compile_source_result_free(&result);xr_compile_resources_release(context.resources);
    const XrXirSourceSyntaxView *escaped=xr_xir_compile_source_snapshot_syntax(clone);
    CHECK(escaped&&escaped->marker_count==4&&escaped->declarations[method].role==XR_XIR_SOURCE_SYNTAX_METHOD);
    xr_xir_compile_source_snapshot_free(clone);lsp_zero();
}
static void lsp_roles_cases(const char *root) {
    size_t sites=0;XrCompileResourceStats needed={0};
    for(size_t probe=0;probe<=sites;++probe) {
        source_fixture_compile_attempts=0;source_fixture_compile_fail_at=probe?probe-1:SIZE_MAX;source_fixture_compile_injected=false;
        XrXirStatus status=roles_once(root,lsp_limits(),probe?NULL:&needed);
        if(!probe){CHECK(status==XR_XIR_OK);sites=source_fixture_compile_attempts;CHECK(sites);}
        else CHECK(status==XR_XIR_OUT_OF_MEMORY&&source_fixture_compile_injected);
    }
    source_fixture_compile_fail_at=SIZE_MAX;source_fixture_compile_injected=false;
    for(unsigned axis=0;axis<3;++axis)for(int delta=-1;delta<=1;++delta) {
        XrCompileResourceLimits limits={needed.allocated_bytes,needed.peak_bytes,needed.work};uint64_t *bound=axis==0?&limits.allocated_bytes:axis==1?&limits.live_bytes:&limits.work;
        CHECK(*bound&&*bound<UINT64_MAX);*bound=(uint64_t)((int64_t)*bound+delta);
        CHECK(roles_once(root,limits,NULL)==(delta<0?XR_XIR_BUDGET:XR_XIR_OK));
    }
    roles_owner_cases(root);
    /* The current Source contract rejects MOVE parameters. Retaining its parser
     * role must not manufacture a Checked query or broaden execution admission. */
    XrCompileResourceLimits limits=lsp_limits();XrXirCompileContext context={0};XrCompilerSession *session=NULL;
    CHECK(xr_compile_resources_new(&limits,&context.resources)==XR_COMPILE_RESOURCE_OK);
    context.limits=xr_xir_compile_default_limits();CHECK(xr_compile_session_new(context.resources,&session)==XR_COMPILER_SESSION_OK);
    XrModuleIdentityAuthority authority={XR_MODULE_IDENTITY_SCRIPT,NULL,root};XrXirSourceRequest request={session,NULL,&authority,&context,NULL,NULL,XR_XIR_PROGRAM,NULL};
    char entry[XR_TEST_PATH_MAX];int entry_length=snprintf(entry,sizeof(entry),"%s/move-rejected.xr",root);
    CHECK(entry_length>0&&(size_t)entry_length<sizeof(entry));request.entry_path=entry;
    static const char rejected[]="fn denied(x:move i64)->i64{return x}\n";XrXirSourceResult result={0};
    CHECK(xr_xir_compile_source_check_text(&request,&(XrXirSourceText){"move-rejected.xr",rejected,sizeof(rejected)-1},&result,NULL,NULL)==XR_XIR_BAD_TYPE);
    CHECK(!result.checked&&!result.snapshot);xr_compile_session_free(session);xr_compile_resources_release(context.resources);lsp_zero();
}
