/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef LSP_RENAME_ADMISSION_CASES_H
#define LSP_RENAME_ADMISSION_CASES_H
static void rename_task_reject(RenameTaskFixture *f,const XrJsonValue *params) {
    size_t blocks=runtime_live,bytes=runtime_bytes,app=task_live;
    XlspHierarchyRequest *request=NULL;
    CHECK(xlsp_hierarchy_request_capture(&f->server,params,0,rename_task_kind(f),&request)==XR_XIR_BAD_STRUCTURE&&!request);
    CHECK(runtime_live==blocks&&runtime_bytes==bytes&&task_live==app);
    XlspPendingRequest *pending=xlsp_pending_request_find(&f->server.pending_requests,&f->id);CHECK(pending&&!pending->source_task_id);
    CHECK(xlsp_source_task_submit(&f->server,&f->id,(XrJsonValue *)params,rename_task_mode(f))==XR_XIR_BAD_STRUCTURE);
    CHECK(!pending->source_task_id&&runtime_live==blocks&&runtime_bytes==bytes&&task_live==app);
}
static void rename_task_admission(RenameTaskFixture *f) {
    const char *keys[]={"textDocument","position","uri","line","character","newName"};
    unsigned duplicates=f->method?5u:6u;
    for(unsigned i=0;i<duplicates;++i) {
        XrJsonValue *params=xjson_clone(f->params);CHECK(params);
        XrJsonValue *object=i<2||i==5?params:i==2?xjson_get_object(params,"textDocument"):xjson_get_object(params,"position");
        CHECK(index_json_member(object,keys[i],xjson_clone(xjson_get(object,keys[i]))));
        rename_task_reject(f,params);xjson_free(params);
    }
    const struct {const char *object,*key,*value;} invalid[]={
        {"textDocument","uri","123"},{"textDocument","uri","\"file:\\u0000bad\""},
        {NULL,"textDocument","[]"},{"position","line","-1"},{"position","line","2147483648"},
        {"position","line","1.5"},{"position","character","false"},{"position","character","-1"},
        {"position","character","2147483648"},{NULL,"position","[]"},
        {NULL,"newName","[]"}, {NULL,"newName","\"\""},{NULL,"newName","\"bad\\u0000name\""}
    };
    unsigned shapes=f->method?10u:(unsigned)(sizeof(invalid)/sizeof(*invalid));
    for(unsigned i=0;i<shapes;++i) {
        XrJsonValue *params=xjson_clone(f->params),*value=xjson_parse(invalid[i].value,strlen(invalid[i].value));CHECK(params&&value);
        XrJsonValue *object=invalid[i].object?xjson_get_object(params,invalid[i].object):params;
        xjson_object_set(object,invalid[i].key,value);CHECK(xjson_get(object,invalid[i].key)==value);
        rename_task_reject(f,params);xjson_free(params);
    }
    const char *uri=xjson_get_string(xjson_get_object(f->params,"textDocument"),"uri");
    XrLspDocument *doc=xlsp_document_store_get(f->server.doc_table,uri);CHECK(doc);doc->server=NULL;
    rename_task_reject(f,f->params);doc->server=&f->server;
    f->server.config.ignore_pattern_count=-1;rename_task_reject(f,f->params);f->server.config.ignore_pattern_count=1;
    for(size_t point=0;point<3;++point) {
        size_t blocks=runtime_live,bytes=runtime_bytes,app=task_live;
        task_fail=task_attempts+point;
        CHECK(xlsp_source_task_submit(&f->server,&f->id,f->params,rename_task_mode(f))==XR_XIR_OUT_OF_MEMORY);
        CHECK(task_attempts==task_fail+1);task_fail=SIZE_MAX;
        XlspPendingRequest *pending=xlsp_pending_request_find(&f->server.pending_requests,&f->id);
        CHECK(pending&&!pending->source_task_id&&runtime_live==blocks&&runtime_bytes==bytes&&task_live==app);
    }
    fprintf(stderr,"RENAME_ADMISSION method=%u duplicates=%u shapes=%u owner_config_rejects=2 app_prefixes=3 physical0/0\n",f->method,duplicates,shapes);
}
#endif // LSP_RENAME_ADMISSION_CASES_H
