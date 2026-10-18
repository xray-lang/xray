/* Copyright (c) 2026 Xinglei Xu. MIT License. */
static bool workspace_hint_before(XrJsonValue *a,XrJsonValue *b) {
    XrJsonValue *ap=xjson_get_object(a,"position"),*bp=xjson_get_object(b,"position");
    int64_t al=xjson_get_int(ap,"line"),bl=xjson_get_int(bp,"line");if(al!=bl)return al<bl;
    int64_t ac=xjson_get_int(ap,"character"),bc=xjson_get_int(bp,"character");if(ac!=bc)return ac<bc;
    return xjson_get_int(a,"kind")<=xjson_get_int(b,"kind");
}
static XrXirStatus workspace_hint_sort(XrCompileResources *resources,XrJsonValue *array) {
    size_t count=(size_t)array->as.array.count;XrJsonValue **scratch=NULL;
    if(count<2)return XR_XIR_OK;if(count>SIZE_MAX/sizeof(*scratch))return XR_XIR_BUDGET;
    XrXirStatus status=workspace_resource(xr_compile_resources_alloc(resources,count*sizeof(*scratch),(void **)&scratch));
    if(status!=XR_XIR_OK)return status;
    for(size_t width=1;width<count;) {
        for(size_t begin=0;begin<count;) {
            size_t middle=width<count-begin?begin+width:count,end=width<count-middle?middle+width:count,left=begin,right=middle;
            for(size_t i=begin;i<end;++i) {
                status=workspace_resource(xr_compile_resources_work(resources,sizeof(*scratch)*2));if(status!=XR_XIR_OK)goto done;
                bool take=right==end||(left<middle&&workspace_hint_before(array->as.array.items[left],array->as.array.items[right]));
                scratch[i]=array->as.array.items[take?left++:right++];
            }
            begin=end;
        }
        status=workspace_resource(xr_compile_resources_work(resources,count*sizeof(*scratch)));if(status!=XR_XIR_OK)goto done;
        memcpy(array->as.array.items,scratch,count*sizeof(*scratch));if(width>count/2)break;width*=2;
    }
done:
    xr_compile_resources_free(scratch);return status;
}
static XrXirStatus workspace_hint_join(XrCompileResources *resources,XrJsonValue *types,XrJsonValue *parameters) {
    if(!types||!parameters||types->type!=XR_JSON_ARRAY||parameters->type!=XR_JSON_ARRAY)return XR_XIR_BAD_STRUCTURE;
    if(parameters->as.array.count>INT_MAX-types->as.array.count)return XR_XIR_BUDGET;
    for(int i=0;i<parameters->as.array.count;++i) {
        XrXirStatus status=workspace_resource(xr_compile_resources_work(resources,sizeof(XrJsonValue *)));if(status!=XR_XIR_OK)return status;
        if(types->as.array.count==types->as.array.capacity&&types->as.array.capacity>INT_MAX/2)return XR_XIR_BUDGET;
        int before=types->as.array.count;xjson_array_push(types,parameters->as.array.items[i]);
        if(types->as.array.count!=before+1)return XR_XIR_OUT_OF_MEMORY;parameters->as.array.items[i]=NULL;
    }
    return workspace_hint_sort(resources,types);
}
XR_FUNC XrXirStatus xlsp_source_workspace_inlay_bindings(XlspSourceWorkspace *workspace,const char *uri,size_t length,
    const XlspSourceInlayOptions *options,XrJsonValue **out) {
    if(!workspace||!uri||length==SIZE_MAX||!options||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XlspSyntaxSnapshot *syntax=NULL;XlspSourceCallArguments *arguments=NULL;XrJsonValue *types=NULL,*parameters=NULL;
    XrXirStatus status=XR_XIR_UNRESOLVED;
    for(size_t i=0;i<workspace->count;++i) {
        bool contains=false;status=xlsp_source_snapshot_contains(workspace->snapshots[i],uri,length,&contains);
        if(status!=XR_XIR_OK)goto done;if(!contains)continue;
        status=xlsp_source_semantic_range(workspace->snapshots[i],uri,length,options->range);if(status!=XR_XIR_OK)goto done;
        XlspSourceDocument document={0};status=xlsp_source_snapshot_document(workspace->snapshots[i],uri,length,&document);
        if(status!=XR_XIR_OK)goto done;
        if(options->types) {
            XlspSourceTypeHintRequest request={workspace->context.resources,workspace->snapshots,workspace->count,document,options->range};
            status=xlsp_source_type_hints_many_json(&request,&types);if(status!=XR_XIR_OK)goto done;
        } else {status=workspace_resource(xr_compile_resources_work(workspace->context.resources,sizeof(XrJsonValue)));if(status!=XR_XIR_OK)goto done;types=xjson_new_array();if(!types){status=XR_XIR_OUT_OF_MEMORY;goto done;}}
        if(options->parameters) {
            XrParseStatus parsed=xlsp_source_syntax_build(workspace->session,&document,&syntax);
            if(parsed!=XR_PARSE_OK&&parsed!=XR_PARSE_RECOVERED) {
                status=parsed==XR_PARSE_BUDGET?XR_XIR_BUDGET:parsed==XR_PARSE_OUT_OF_MEMORY?XR_XIR_OUT_OF_MEMORY:XR_XIR_BAD_STRUCTURE;goto done;
            }
            status=xlsp_source_syntax_arguments(syntax,&arguments);if(status!=XR_XIR_OK)goto done;
            XlspSourceParameterHintRequest request={workspace->context.resources,workspace->snapshots,workspace->count,arguments,options->range};
            status=xlsp_source_parameter_hints_many_json(&request,&parameters);if(status!=XR_XIR_OK)goto done;
            status=workspace_hint_join(workspace->context.resources,types,parameters);if(status!=XR_XIR_OK)goto done;
        } else {status=workspace_hint_sort(workspace->context.resources,types);if(status!=XR_XIR_OK)goto done;}
        *out=types;types=NULL;status=XR_XIR_OK;goto done;
    }
    status=XR_XIR_UNRESOLVED;
done:
    xjson_free(types);xjson_free(parameters);xlsp_source_call_arguments_free(arguments);xlsp_source_syntax_free(syntax);return status;
}
