/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#define xlsp_source_type_hints_many_json type_strict_many_json
#include "../../../../src/app/lsp/xlsp_source_type_json.c"
#undef xlsp_source_type_hints_many_json
/* Counterexample: visible spelling/position equality discards the actual type owner. */
XR_FUNC XrXirStatus xlsp_source_type_hints_many_json(const XlspSourceTypeHintRequest *request,XrJsonValue **out) {
    XlspSourceTypeHints accepted={0},current={0};bool selected=false;XrXirStatus status=XR_XIR_OK;
    for(size_t i=0;i<request->count;++i) {
        bool contains=false;status=xlsp_source_snapshot_contains(request->snapshots[i],request->document.uri,request->document.uri_length,&contains);
        if(status!=XR_XIR_OK)goto done;if(!contains)continue;
        status=xlsp_source_type_hints(request->snapshots[i],request->document.uri,request->document.uri_length,request->range,&current);
        if(status!=XR_XIR_OK)goto done;
        if(selected) {
            if(accepted.count!=current.count){status=XR_XIR_BAD_STRUCTURE;goto done;}
            for(size_t n=0;n<accepted.count;++n)if(strcmp(accepted.items[n].label,current.items[n].label)||
                accepted.items[n].position.line!=current.items[n].position.line||accepted.items[n].position.character!=current.items[n].position.character) {
                status=XR_XIR_BAD_STRUCTURE;goto done;
            }
            xlsp_source_type_hints_free(&current);
        } else {accepted=current;current=(XlspSourceTypeHints){0};selected=true;}
    }
    status=selected?type_json_render(request->resources,&accepted,out):XR_XIR_UNRESOLVED;
done:
    xlsp_source_type_hints_free(&accepted);xlsp_source_type_hints_free(&current);return status;
}
#include "test_type_json.c"
