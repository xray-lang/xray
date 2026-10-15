/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static void protocol_case(char **argv) {
    CallFixture f;call_fixture(&f,argv,root_text);XrJsonValue *prepared=call_prepared(&f,1,3),*wrapper=xjson_array_get(prepared,0);
    for(unsigned mode=0;mode<18;++mode){XrJsonValue *p=mode<7||mode>=16?prepare_params(argv[1],1,3):item_params(wrapper);
        unsigned method=mode<7||mode>=16?0u:2u;XrXirStatus expected=XR_XIR_BAD_STRUCTURE;
        XrJsonValue *position=xjson_get_object(p,"position"),*item=xjson_get_object(p,"item"),*data=xjson_get_object(item,"data");
        switch(mode){
        case 0:xjson_object_set(position,"line",xjson_new_number(1));break;
        case 1:{XrJsonValue *negative=integer(0);negative->as.integer=-1;xjson_object_set(position,"line",negative);break;}
        case 2:xjson_object_set(position,"line",integer(UINT32_C(0x80000000)));break;
        case 3:xjson_object_set(position,"character",integer(UINT32_C(0x80000000)));break;
        case 4:xjson_object_set(position,"line",integer(0));xjson_object_set(position,"character",integer(4));break;
        case 5:xjson_object_set_new(position,"line",integer(1));break;
        case 6:xjson_object_set(xjson_get_object(p,"textDocument"),"uri",xjson_new_string_n("file:///x\0bad",13));break;
        case 7:xjson_object_set(item,"data",xjson_new_null());break;
        case 8:xjson_object_set(data,"revision",xjson_new_string("0000000000000000"));expected=XR_XIR_UNRESOLVED;break;
        case 9:xjson_object_set(data,"fingerprint",xjson_new_string("0000000000000000000000000000000000000000000000000000000000000000"));expected=XR_XIR_UNRESOLVED;break;
        case 10:xjson_object_set(data,"version",xjson_new_string("8"));expected=XR_XIR_UNRESOLVED;break;
        case 11:xjson_object_set(item,"name",xjson_new_string("target"));expected=XR_XIR_UNRESOLVED;break;
        case 12:xjson_object_set(item,"kind",xjson_new_number(LSP_SYMBOL_FUNCTION));break;
        case 13:xjson_object_set(xjson_get_object(xjson_get_object(item,"range"),"end"),"character",integer(123));expected=XR_XIR_UNRESOLVED;break;
        case 14:xjson_object_set(data,"moduleInitializer",xjson_new_bool(true));expected=XR_XIR_UNRESOLVED;break;
        case 15:xjson_object_set_new(data,"schema",xjson_new_string("xray-source-call-1"));break;
        case 16:xjson_object_set(position,"line",integer(0));xjson_object_set(position,"character",integer(0));expected=XR_XIR_UNRESOLVED;break;
        case 17:f.server.shutdown_received=true;break;
        }
        XrJsonValue *result=method?xlsp_handle_call_hierarchy_outgoing(&f.server,p):xlsp_handle_prepare_call_hierarchy(&f.server,p);
        if(result||f.server.source_failure.status!=(unsigned)expected)fprintf(stderr,"protocol case%u expected%u got%u\n",mode,(unsigned)expected,f.server.source_failure.status);
        CHECK(!result&&f.server.source_failure.status==(unsigned)expected&&f.server.source_failure.stage==24&&!runtime_live&&!runtime_bytes);xjson_free(p);
    }
    call_fixture_done(&f);xjson_free(prepared);CHECK(!lsp_json_live&&!lsp_json_bytes);
    puts("Actual call protocol: 18 strict integer/cursor/URI/duplicate/stale item/shutdown rejections, no partial output");
}
