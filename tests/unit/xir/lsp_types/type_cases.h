/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void unicode_case(char **argv) {
    TypeFixture f;fixture(&f,argv,"/* \xf0\x9f\x98\x80 */ interface Here {};\rinterface Child extends Here {};\r");
    XrJsonValue *prepared=prepare(&f,0,19),*here=xjson_array_get(prepared,0);CHECK(named(here,"Here"));range(here,"selectionRange",0,19,0,23);range(here,"range",0,19,0,26);
    XrJsonValue *children=related(&f,here,2);CHECK(children->as.array.count==1&&named(xjson_array_get(children,0),"Child"));range(xjson_array_get(children,0),"selectionRange",1,10,1,15);
    XrJsonValue *parents=related(&f,xjson_array_get(children,0),1);CHECK(parents->as.array.count==1&&named(xjson_array_get(parents,0),"Here"));
    done(&f);xjson_free(prepared);xjson_free(children);xjson_free(parents);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual type UTF16: astral prefix, standalone CR and exact source definition/selection conversion");
}
static void generic_case(char **argv) {
    TypeFixture f;fixture(&f,argv,"interface Reader<T:Sendable> {read()->T}\nstruct Box<T:Sendable> implements Reader<T> {value:T;read()->T{return this.value}}\n");
    XrJsonValue *prepared=prepare(&f,0,11),*reader=xjson_array_get(prepared,0),*children=related(&f,reader,2);
    CHECK(children->as.array.count==1&&named(xjson_array_get(children,0),"Box"));XrJsonValue *parents=related(&f,xjson_array_get(children,0),1);
    CHECK(parents->as.array.count==1&&named(xjson_array_get(parents,0),"Reader"));done(&f);xjson_free(prepared);xjson_free(children);xjson_free(parents);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual generic type hierarchy: canonical interface application and declaration identities, no instantiated ID persisted");
}
static void homonym_case(char **argv) {
    TypeFixture f;fixture(&f,argv,"import {Base as Alias} from \"./lib\"\ninterface Base {}\ninterface Local extends Base {}\ninterface Foreign extends Alias {}\n");
    XrJsonValue *prepared=prepare(&f,1,11),*children=related(&f,xjson_array_get(prepared,0),2);CHECK(children->as.array.count==1&&named(xjson_array_get(children,0),"Local"));
    XrJsonValue *parents=related(&f,xjson_array_get(children,0),1);CHECK(parents->as.array.count==1&&!strcmp(xjson_get_string(xjson_array_get(parents,0),"uri"),argv[1]));
    xjson_free(prepared);xjson_free(children);xjson_free(parents);prepared=prepare(&f,3,11);parents=related(&f,xjson_array_get(prepared,0),1);
    CHECK(parents->as.array.count==1&&named(xjson_array_get(parents,0),"Base")&&!strcmp(xjson_get_string(xjson_array_get(parents,0),"uri"),argv[2]));
    children=related(&f,xjson_array_get(parents,0),2);CHECK(children->as.array.count==2);find(children,"Foreign");find(children,"External");
    done(&f);xjson_free(prepared);xjson_free(children);xjson_free(parents);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual type homonyms: same source spelling in distinct modules remains separate under alias and closed-file queries");
}
static void protocol_case(char **argv) {
    TypeFixture f;fixture(&f,argv,root_text);XrJsonValue *prepared=prepare(&f,1,11),*left=xjson_array_get(prepared,0);
    char changed[sizeof(root_text)+16];CHECK(snprintf(changed,sizeof(changed),"%s// changed\n",root_text)>0);
    for(unsigned mode=0;mode<19;++mode){bool select=mode<7||mode==16||mode==17;XrJsonValue *p=select?params(argv[1],1,11):item_params(left);
        XrJsonValue *at=xjson_get_object(p,"position"),*item=xjson_get_object(p,"item"),*data=xjson_get_object(item,"data");XrXirStatus expected=XR_XIR_BAD_STRUCTURE;
        switch(mode){
        case 0:xjson_object_set(at,"line",xjson_new_number(1));break;
        case 1:{XrJsonValue *v=integer(0);v->as.integer=-1;xjson_object_set(at,"line",v);break;}
        case 2:xjson_object_set(at,"line",integer(UINT32_C(0x80000000)));break;
        case 3:xjson_object_set(at,"character",integer(UINT32_C(0x80000000)));break;
        case 4:xjson_object_set(at,"line",integer(0));xjson_object_set(at,"character",integer(4));break;
        case 5:xjson_object_set_new(at,"line",integer(1));break;
        case 6:xjson_object_set(xjson_get_object(p,"textDocument"),"uri",xjson_new_string_n("file:///x\0bad",13));break;
        case 7:xjson_object_set(item,"data",xjson_new_null());break;
        case 8:xjson_object_set(data,"revision",xjson_new_string("0000000000000000"));expected=XR_XIR_UNRESOLVED;break;
        case 9:xjson_object_set(data,"fingerprint",xjson_new_string("0000000000000000000000000000000000000000000000000000000000000000"));expected=XR_XIR_UNRESOLVED;break;
        case 10:xjson_object_set(data,"version",xjson_new_string("8"));expected=XR_XIR_UNRESOLVED;break;
        case 11:xjson_object_set(item,"name",xjson_new_string("Base"));expected=XR_XIR_UNRESOLVED;break;
        case 12:xjson_object_set(item,"kind",xjson_new_number(LSP_SYMBOL_INTERFACE));break;
        case 13:xjson_object_set(xjson_get_object(xjson_get_object(item,"range"),"end"),"character",integer(99));expected=XR_XIR_UNRESOLVED;break;
        case 14:xjson_object_set(data,"schema",xjson_new_string("xray-source-call-1"));expected=XR_XIR_UNRESOLVED;break;
        case 15:xjson_object_set_new(data,"schema",xjson_new_string("xray-source-type-1"));break;
        case 16:xjson_object_set(at,"line",integer(0));xjson_object_set(at,"character",integer(0));expected=XR_XIR_UNRESOLVED;break;
        case 17:f.server.shutdown_received=true;break;
        case 18:f.server.shutdown_received=false;f.root.content=changed;f.root.length=strlen(changed);expected=XR_XIR_UNRESOLVED;break;
        }
        XrJsonValue *result=select?xlsp_handle_prepare_type_hierarchy(&f.server,p):xlsp_handle_type_hierarchy_supertypes(&f.server,p);
        if(result||f.server.source_failure.status!=(unsigned)expected)fprintf(stderr,"type protocol%u expected%u actual%u\n",mode,(unsigned)expected,f.server.source_failure.status);
        CHECK(!result&&f.server.source_failure.stage==25&&f.server.source_failure.status==(unsigned)expected&&!runtime_live&&!runtime_bytes);xjson_free(p);
    }
    done(&f);xjson_free(prepared);CHECK(!lsp_json_live&&!lsp_json_bytes);puts("Actual type protocol:19 integer/cursor/URI/duplicate/stale/schema/shutdown/same-version-edit complete rejections");
}
static void inheritance_required(char **argv) {
    TypeFixture f;fixture(&f,argv,"class Base {}\nclass Child extends Base {}\n");XrJsonValue *prepared=prepare(&f,0,6);
    XrJsonValue *children=related(&f,xjson_array_get(prepared,0),2);CHECK(children->as.array.count==1&&named(xjson_array_get(children,0),"Child"));
    done(&f);xjson_free(prepared);xjson_free(children);CHECK(!lsp_json_live&&!lsp_json_bytes);
}
static void qualified_case(char **argv) {
    TypeFixture f;fixture(&f,argv,"import \"./lib\" as ns\ninterface Qualified extends ns  .  Base {}\n");
    XrJsonValue *prepared=prepare(&f,1,35),*base=xjson_array_get(prepared,0);
    CHECK(named(base,"Base")&&!strcmp(xjson_get_string(base,"uri"),argv[2]));
    XrJsonValue *children=related(&f,base,2);CHECK(children->as.array.count==2);find(children,"Qualified");find(children,"External");
    XrJsonValue *p=params(argv[1],1,32);CHECK(!xlsp_handle_prepare_type_hierarchy(&f.server,p)&&f.server.source_failure.status==XR_XIR_UNRESOLVED);
    done(&f);xjson_free(p);xjson_free(prepared);xjson_free(children);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual qualified type use: exact member token across whitespace, dot excluded, canonical cross-file interface");
}
