/* Copyright (c) 2026 Xinglei Xu. MIT License. */
typedef struct CallFixture {XrLspServer server;XrLspDocument root;} CallFixture;
static void call_fixture(CallFixture *f,char **argv,const char *text) {
    memset(f,0,sizeof(*f));f->server.doc_table=xlsp_document_store_new(destroyed);CHECK(f->server.doc_table);f->server.source_revision=101;
    f->root.server=&f->server;f->root.uri=argv[1];f->root.content=(char *)text;f->root.length=strlen(text);f->root.version=9;f->root.ast=(AstNode *)(uintptr_t)1;
    XlspDocumentInsertion insert={0};CHECK(xlsp_document_store_prepare(f->server.doc_table,f->root.uri,&f->root,&insert)&&xlsp_document_store_commit(&insert));
}
static XrJsonValue *call_prepared(CallFixture *f,unsigned line,unsigned column) {
    XrJsonValue *params=prepare_params(f->root.uri,line,column),*result=invoke(&f->server,params,0);xjson_free(params);CHECK(result->as.array.count==1);return result;
}
static XrJsonValue *call_related(CallFixture *f,XrJsonValue *item,unsigned mode) {
    XrJsonValue *params=item_params(item),*result=invoke(&f->server,params,mode);xjson_free(params);return result;
}
static void call_fixture_done(CallFixture *f){CHECK(f->root.ast==(AstNode *)(uintptr_t)1);xlsp_document_store_free(f->server.doc_table);CHECK(!runtime_live&&!runtime_bytes);}
static void closure_case(char **argv) {
    const char *text="fn target(v:i64)->i64{return v}\nfn outer()->fn(i64)->i64{return fn(v:i64)->i64{return target(v)}}\nconst cb=outer()\n";
    CallFixture f;call_fixture(&f,argv,text);XrJsonValue *prepared=call_prepared(&f,0,3),*item=xjson_array_get(prepared,0);
    CHECK(!strcmp(xjson_get_string(item,"uri"),argv[1]));XrJsonValue *incoming=call_related(&f,item,1);CHECK(incoming->as.array.count==1);
    XrJsonValue *closure=xjson_get_object(xjson_array_get(incoming,0),"from");CHECK(named(closure,"anonymous function"));
    XrJsonValue *selection=xjson_get_object(closure,"selectionRange"),*a=xjson_get_object(selection,"start"),*b=xjson_get_object(selection,"end");
    CHECK(xjson_get_int(a,"line")==1&&xjson_get_int(b,"line")==1&&xjson_get_int(a,"character")==xjson_get_int(b,"character"));
    XrJsonValue *outgoing=call_related(&f,closure,2);CHECK(outgoing->as.array.count==1);
    CHECK(named(xjson_get_object(xjson_array_get(outgoing,0),"to"),"target"));call_fixture_done(&f);
    CHECK(!strcmp(xjson_get_string(xjson_get_object(xjson_array_get(outgoing,0),"to"),"uri"),argv[1]));
    xjson_free(prepared);xjson_free(incoming);xjson_free(outgoing);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual call closure: canonical homonym separation, duplicate import-closure suppression, actual unnamed body, owned round trip");
}
static void recursion_case(char **argv) {
    CallFixture f;call_fixture(&f,argv,"fn target(v:i64)->i64{if(v==0){return 0};return target(v-1)}\nconst a=target(2)\n");
    XrJsonValue *prepared=call_prepared(&f,0,3),*item=xjson_array_get(prepared,0),*incoming=call_related(&f,item,1);
    CHECK(incoming->as.array.count==2);XrJsonValue *self=find_edge(incoming,"from","target");CHECK(xjson_get(self,"fromRanges")->as.array.count==1);
    XrJsonValue *outgoing=call_related(&f,item,2);CHECK(outgoing->as.array.count==1&&named(xjson_get_object(xjson_array_get(outgoing,0),"to"),"target"));
    call_fixture_done(&f);xjson_free(prepared);xjson_free(incoming);xjson_free(outgoing);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual call recursion: exact self declaration and independent module caller");
}
static void method_case(char **argv) {
    CallFixture f;call_fixture(&f,argv,"fn target(v:i64)->i64{return v}\nstruct S {\n value:i64\n static pick(v:i64)->i64{return target(v)}\n}\nconst a=S.pick(1)\n");
    XrJsonValue *prepared=call_prepared(&f,5,10),*method=xjson_array_get(prepared,0);CHECK(named(method,"pick")&&xjson_get_int(method,"kind")==LSP_SYMBOL_METHOD);
    range(method,"selectionRange",3,8,3,12);XrJsonValue *outgoing=call_related(&f,method,2);CHECK(outgoing->as.array.count==1);
    CHECK(!strcmp(xjson_get_string(xjson_get_object(xjson_array_get(outgoing,0),"to"),"uri"),argv[1]));
    call_fixture_done(&f);xjson_free(prepared);xjson_free(outgoing);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual call method: member-selected declaration and real method item");
}
static void utf16_case(char **argv) {
    CallFixture f;call_fixture(&f,argv,"/* \xf0\x9f\x98\x80 */ fn target(v:i64)->i64{return v};\rfn wrapper(v:i64)->i64{return target(v)};\rconst a=wrapper(1);\r");
    XrJsonValue *prepared=call_prepared(&f,0,12),*target=xjson_array_get(prepared,0);range(target,"selectionRange",0,12,0,18);
    XrJsonValue *incoming=call_related(&f,target,1);CHECK(incoming->as.array.count==1);XrJsonValue *wrapper=xjson_get_object(xjson_array_get(incoming,0),"from");
    range(wrapper,"selectionRange",1,3,1,10);XrJsonValue *outgoing=call_related(&f,wrapper,2);CHECK(outgoing->as.array.count==1);
    range(xjson_get_object(xjson_array_get(outgoing,0),"to"),"selectionRange",0,12,0,18);
    call_fixture_done(&f);xjson_free(prepared);xjson_free(incoming);xjson_free(outgoing);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual call UTF16: astral prefix and standalone CR Source/LSP coordinates");
}
static void indirect_case(char **argv) {
    CallFixture f;call_fixture(&f,argv,"fn target(v:i64)->i64{return v}\nfn run(cb:fn(i64)->i64,v:i64)->i64{return cb(v)}\nconst a=run(target,1)\n");
    XrJsonValue *prepared=call_prepared(&f,0,3),*target=xjson_array_get(prepared,0),*params=item_params(target);
    CHECK(!xlsp_handle_call_hierarchy_incoming(&f.server,params)&&f.server.source_failure.status==XR_XIR_UNSUPPORTED);xjson_free(params);
    XrJsonValue *empty=call_related(&f,target,2);CHECK(!empty->as.array.count);xjson_free(empty);xjson_free(prepared);
    prepared=call_prepared(&f,1,3);params=item_params(xjson_array_get(prepared,0));
    CHECK(!xlsp_handle_call_hierarchy_outgoing(&f.server,params)&&f.server.source_failure.status==XR_XIR_UNSUPPORTED);xjson_free(params);
    call_fixture_done(&f);xjson_free(prepared);CHECK(!runtime_live&&!runtime_bytes&&!lsp_json_live&&!lsp_json_bytes);
    puts("Actual call indirect: explicit target absence fails complete incoming/selected outgoing; real unused outgoing stays empty");
}
static void local_closure_case(char **argv) {
    const char *text="fn target(v:i64)->i64{return v}\nfn outer()->fn(i64)->i64{return fn(v:i64)->i64{return target(v)}}\nfn keep(){const cb=outer()}\n";
    CallFixture f;call_fixture(&f,argv,text);XrJsonValue *prepared=call_prepared(&f,0,3),*incoming=call_related(&f,xjson_array_get(prepared,0),1);
    CHECK(incoming->as.array.count==1);XrJsonValue *closure=xjson_get_object(xjson_array_get(incoming,0),"from");CHECK(named(closure,"anonymous function"));
    XrJsonValue *outgoing=call_related(&f,closure,2);CHECK(outgoing->as.array.count==1&&named(xjson_get_object(xjson_array_get(outgoing,0),"to"),"target"));
    call_fixture_done(&f);CHECK(!strcmp(xjson_get_string(xjson_get_object(xjson_array_get(outgoing,0),"to"),"uri"),argv[1]));
    xjson_free(prepared);xjson_free(incoming);xjson_free(outgoing);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual local closure: Source body identity across root closures, exact item round trip, no module-slot substitution");
}
