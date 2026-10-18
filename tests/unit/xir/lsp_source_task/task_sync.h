/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_SOURCE_TASK_SYNC_H
#define LSP_SOURCE_TASK_SYNC_H
#include "app/lsp/xlsp_source_workspace.h"
static void check_sync_navigation(XrLspServer *server,XrJsonValue *params,const char *library_uri) {
    size_t json=lsp_json_live,bytes=lsp_json_bytes;
    for(unsigned mode=0;mode<4;++mode){
        XrJsonValue *out=xlsp_navigation_handle(server,params,mode);
        CHECK(out&&server->source_workspace&&!server->source_failure.stage&&server->source_failure.status==XR_XIR_OK);
        xlsp_source_workspace_free(server->source_workspace);server->source_workspace=NULL;
        CHECK(!runtime_live&&!runtime_bytes);
        if(mode==0)CHECK(!strcmp(xjson_get_string(out,"uri"),library_uri));
        else if(mode<3)CHECK(out->type==XR_JSON_ARRAY&&out->as.array.count==1);
        else CHECK(strstr(xjson_get_string(xjson_get_object(out,"contents"),"value"),"value"));
        xjson_free(out);CHECK(lsp_json_live==json&&lsp_json_bytes==bytes);
    }
    fprintf(stderr,"Actual synchronous Source navigation4 modes strict64Mi8Mi128000000 and output owner death PASS\n");
}
#endif
