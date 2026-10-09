/* Shared with navigation's checked, strong JSON publication helpers. */
XrXirStatus xlsp_source_hover_many_json(XrCompileResources *resources,
    XlspSourceSnapshot *const *snapshots,size_t count,const char *uri,size_t length,
    XrLspPosition position,XrJsonValue **output) {
    if(!resources||!snapshots||!count||!uri||length==SIZE_MAX||!output||*output)return XR_XIR_BAD_STRUCTURE;
    XlspSourceHover accepted={0},current={0};XlspSourceLocation definition={0};bool found=false;
    XrJsonValue *contents=NULL,*result=NULL;XrXirStatus status=XR_XIR_OK;
    for(size_t i=0;i<count;++i) {
        bool contains=false;status=xlsp_source_snapshot_contains(snapshots[i],uri,length,&contains);
        if(status!=XR_XIR_OK)goto done;if(!contains)continue;
        XlspSourceLocation location={0};
        status=xlsp_source_definition(snapshots[i],uri,length,position,&location);if(status!=XR_XIR_OK)goto done;
        status=xlsp_source_hover(snapshots[i],uri,length,position,&current);if(status!=XR_XIR_OK)goto done;
        if(found) {
            bool equal=false;status=navigation_string_equal(resources,definition.uri,location.uri,&equal);if(status!=XR_XIR_OK)goto done;
            if(xr_compile_resources_work(resources,10)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
            if(!equal||!navigation_same_range(definition.range,location.range)||definition.version!=location.version||accepted.length!=current.length){status=XR_XIR_BAD_STRUCTURE;goto done;}
            if(xr_compile_resources_work(resources,accepted.length)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
            if(memcmp(accepted.text,current.text,accepted.length)){status=XR_XIR_BAD_STRUCTURE;goto done;}
            xlsp_source_hover_free(&current);
        } else {definition=location;accepted=current;current=(XlspSourceHover){0};found=true;}
    }
    if(!found){status=XR_XIR_UNRESOLVED;goto done;}
    /* Plain text carries the exact owned rendering, without Markdown escaping
     * or a fabricated occurrence range. LSP permits an omitted Hover.range. */
    contents=xjson_new_object();if(!contents){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    if(!navigation_member(contents,"kind",xjson_new_string("plaintext"))||
        !navigation_member(contents,"value",xjson_new_string(accepted.text))){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    result=xjson_new_object();if(!result){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    {XrJsonValue *transferred=contents;contents=NULL;
    if(!navigation_member(result,"contents",transferred)){status=XR_XIR_OUT_OF_MEMORY;goto done;}}
    *output=result;result=NULL;
done:
    xjson_free(contents);xjson_free(result);xlsp_source_hover_free(&current);xlsp_source_hover_free(&accepted);return status;
}
