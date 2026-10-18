/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#define xlsp_source_parameter_hints_many_json parameter_strict_many_json
#include "../../../../src/app/lsp/xlsp_source_parameter_json.c"
#undef xlsp_source_parameter_hints_many_json
/* Counterexample: compare only the visible JSON and discard target provenance. */
static bool parameter_visible_equal(const XrJsonValue *a,const XrJsonValue *b) {
    if(a->type!=XR_JSON_ARRAY||b->type!=XR_JSON_ARRAY||a->as.array.count!=b->as.array.count)return false;
    for(int i=0;i<a->as.array.count;++i) {
        XrJsonValue *left=a->as.array.items[i],*right=b->as.array.items[i];
        if(strcmp(xjson_get_string(left,"label"),xjson_get_string(right,"label")))return false;
        XrJsonValue *lp=xjson_get_object(left,"position"),*rp=xjson_get_object(right,"position");
        if(xjson_get_int(lp,"line")!=xjson_get_int(rp,"line")||xjson_get_int(lp,"character")!=xjson_get_int(rp,"character"))return false;
        const char *lt=xjson_get_string(left,"tooltip"),*rt=xjson_get_string(right,"tooltip");
        if(!!lt!=!!rt||(lt&&strcmp(lt,rt)))return false;
    }
    return true;
}
XR_FUNC XrXirStatus xlsp_source_parameter_hints_many_json(const XlspSourceParameterHintRequest *request,XrJsonValue **out) {
    XrJsonValue *accepted=NULL;XrXirStatus status=XR_XIR_UNRESOLVED;
    const XlspSourceCallArgumentsView *input=xlsp_source_call_arguments_view(request->arguments);
    for(size_t i=0;i<request->count;++i) {
        bool contains=false;status=xlsp_source_snapshot_contains(request->snapshots[i],input->uri,input->uri_length,&contains);
        if(status!=XR_XIR_OK)goto done;if(!contains)continue;
        XlspSourceParameterHints hints={0};XrJsonValue *current=NULL;
        status=xlsp_source_parameter_hints(request->snapshots[i],request->arguments,request->range,&hints);
        if(status==XR_XIR_OK)status=parameter_json_render(request->resources,&hints,&current);
        xlsp_source_parameter_hints_free(&hints);if(status!=XR_XIR_OK)goto done;
        if(accepted){bool same=parameter_visible_equal(current,accepted);xjson_free(current);if(!same){status=XR_XIR_BAD_STRUCTURE;goto done;}}
        else accepted=current;
    }
    if(accepted){*out=accepted;accepted=NULL;status=XR_XIR_OK;}
done:
    xjson_free(accepted);return status;
}
#include "test_parameter_json.c"
