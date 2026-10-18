/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef PARAMETER_HINT_PROVENANCE_H
#define PARAMETER_HINT_PROVENANCE_H
static void parameter_same_text(XlspSourceHover a,XlspSourceHover b) {
    CHECK(a.length==b.length&&!!a.text==!!b.text);if(a.length)CHECK(!memcmp(a.text,b.text,a.length));
}
static void parameter_same_hints(const XlspSourceParameterHints *a,const XlspSourceParameterHints *b) {
    CHECK(a->count==b->count);
    for(size_t i=0;i<a->count;++i) {
        const XlspSourceParameterHint *left=&a->items[i],*right=&b->items[i];
        parameter_same_text(left->label,right->label);parameter_same_text(left->target_identity,right->target_identity);
        parameter_same_text(left->target_path,right->target_path);
        parameter_same_text(left->target_uri,right->target_uri);
        CHECK(left->position.line==right->position.line&&left->position.character==right->position.character&&
            left->mode==right->mode&&left->target_native_identity==right->target_native_identity&&left->target_kind==right->target_kind);
        CHECK(left->has_target_location==right->has_target_location&&left->target_version==right->target_version);
        CHECK(left->target_selection.start.line==right->target_selection.start.line&&
            left->target_selection.start.character==right->target_selection.start.character&&
            left->target_selection.end.line==right->target_selection.end.line&&left->target_selection.end.character==right->target_selection.end.character);
        CHECK(!memcmp(&left->target_fingerprint,&right->target_fingerprint,sizeof(XrFingerprint)));
        CHECK(left->target_range.module==right->target_range.module&&left->target_range.line==right->target_range.line&&
            left->target_range.column==right->target_range.column&&left->target_range.end_line==right->target_range.end_line&&
            left->target_range.end_column==right->target_range.end_column);
    }
}
static XlspSourceWorkspace *parameter_workspace_pair(XrCompileResources *resources,const char *uri,const char *library,
    const char *program,const char *dependency) {
    XlspSourceDocument documents[]={{uri,program,strlen(uri),strlen(program),9},
        {library,dependency,strlen(library),strlen(dependency),7}};
    XrXirCompileContext context={resources,xr_xir_compile_default_limits()};XlspSourceWorkspace *workspace=NULL;
    unsigned stage=0;XlspSourceWorkspaceFailure failure={0};
    XrXirStatus status=xlsp_source_workspace_build(&context,documents,2,&workspace,&stage,&failure);
    if(status!=XR_XIR_OK)fprintf(stderr,"parameter paired stage=%u status=%u %s\n",stage,status,failure.diagnostic.message);
    CHECK(status==XR_XIR_OK);xlsp_source_workspace_failure_free(&failure);return workspace;
}
static void parameter_crossroots(const char *uri,const char *library,const XrCompileResourceLimits *limits) {
    const char *source="export fn helper(value:i64)->i64{return value}\nexport fn compute(input:i64)->i64{return helper(input)}\nfn main()->i64{return compute(1)}\n";
    const char *dependency="import { compute as renamed } from \"./root\"\nexport fn mix(value:i64)->i64{return renamed(value)}\n";
    const char *changed="export fn helper(value:i64)->i64{return value+1}\nexport fn compute(input:i64)->i64{return helper(input)}\nfn main()->i64{return compute(1)}\n";
    XrCompileResources *resources=sdk_ledger(limits);XlspSourceWorkspace *workspace=parameter_workspace_pair(resources,uri,library,source,dependency);
    XlspSourceCallArguments *arguments=parameter_syntax(resources,uri,source,9);
    XlspSourceSnapshot *snapshots[]={workspace->snapshots[0],workspace->snapshots[1]};
    workspace->snapshots[0]=workspace->snapshots[1]=NULL;xlsp_source_workspace_free(workspace);
    XlspSourceParameterHints hints[2]={{0}};XrLspRange range=parameter_range(source);
    for(size_t i=0;i<2;++i)CHECK(xlsp_source_parameter_hints(snapshots[i],arguments,range,&hints[i])==XR_XIR_OK);
    CHECK(hints[0].count==2&&!strcmp(hints[0].items[0].label.text,"value:")&&!strcmp(hints[0].items[1].label.text,"input:"));
    parameter_same_hints(&hints[0],&hints[1]);
    XlspSourceCallArguments *alias_arguments=parameter_syntax(resources,library,dependency,7);XlspSourceParameterHints alias={0};
    CHECK(xlsp_source_parameter_hints(snapshots[1],alias_arguments,parameter_range(dependency),&alias)==XR_XIR_OK&&alias.count==1);
    parameter_same_text(alias.items[0].label,hints[0].items[1].label);
    parameter_same_text(alias.items[0].target_identity,hints[0].items[1].target_identity);
    CHECK(alias.items[0].target_range.line==hints[0].items[1].target_range.line&&
        alias.items[0].target_range.column==hints[0].items[1].target_range.column);
    workspace=parameter_workspace_pair(resources,uri,library,changed,dependency);XlspSourceParameterHints different={0};
    CHECK(xlsp_source_parameter_hints(workspace->snapshots[1],alias_arguments,parameter_range(dependency),&different)==XR_XIR_OK&&different.count==1);
    parameter_same_text(alias.items[0].label,different.items[0].label);
    parameter_same_text(alias.items[0].target_identity,different.items[0].target_identity);
    parameter_same_text(alias.items[0].target_path,different.items[0].target_path);
    CHECK(memcmp(&alias.items[0].target_fingerprint,&different.items[0].target_fingerprint,sizeof(XrFingerprint)));
    xlsp_source_workspace_free(workspace);xlsp_source_parameter_hints_free(&different);xlsp_source_parameter_hints_free(&alias);
    xlsp_source_call_arguments_free(alias_arguments);
    xlsp_source_call_arguments_free(arguments);for(size_t i=0;i<2;++i)xlsp_source_snapshot_free(snapshots[i]);
    xr_compile_resources_release(resources);parameter_same_hints(&hints[0],&hints[1]);
    for(size_t i=0;i<2;++i)xlsp_source_parameter_hints_free(&hints[i]);CHECK(!runtime_live&&!runtime_bytes);
}
static size_t parameter_long(const char *uri,const XrCompileResourceLimits *limits,bool failures) {
    char name[97],program[400],label[98];memset(name,'p',sizeof(name)-1);name[96]=0;
    int written=snprintf(program,sizeof(program),"fn identity(%s:i64)->i64{return %s}\nfn main()->i64{return identity(1)}\n",name,name);
    CHECK(written>0&&(size_t)written<sizeof(program));memcpy(label,name,96);label[96]=':';label[97]=0;
    XrCompileResources *resources=sdk_ledger(limits);XlspSourceCallArguments *arguments=NULL;
    XlspSourceSnapshot *snapshot=parameter_producer(resources,uri,program,&arguments);XlspSourceParameterHints hints={0};
    ParameterHintCase test={.source=program,.hints={{label,XR_PARAM_READ,0,XR_XIR_SOURCE_FUNCTION}},.count=1};
    size_t sites=parameter_attachment(resources,snapshot,arguments,&test,failures);
    CHECK(xlsp_source_parameter_hints(snapshot,arguments,parameter_range(program),&hints)==XR_XIR_OK&&hints.count==1);
    CHECK(hints.items[0].label.length==97&&!strcmp(hints.items[0].label.text,label));
    xlsp_source_call_arguments_free(arguments);xlsp_source_snapshot_free(snapshot);xr_compile_resources_release(resources);
    CHECK(!strcmp(hints.items[0].label.text,label));xlsp_source_parameter_hints_free(&hints);CHECK(!runtime_live&&!runtime_bytes);return sites;
}
#endif
