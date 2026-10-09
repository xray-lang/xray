/* Actual LSP full/delta/range JSON; cache publication belongs to the caller. */
#include <stdio.h>
static XrXirStatus tokens_array_push(XrJsonValue *array,XrJsonValue *item) {
    if(!item)return XR_XIR_OUT_OF_MEMORY;
    if(array->as.array.count==array->as.array.capacity&&array->as.array.capacity>INT_MAX/2){xjson_free(item);return XR_XIR_BUDGET;}
    int before=array->as.array.count;xjson_array_push(array,item);
    if(array->as.array.count!=before+1){xjson_free(item);return XR_XIR_OUT_OF_MEMORY;}return XR_XIR_OK;
}
static uint32_t tokens_word(const XlspSourceToken *t,size_t index,unsigned field) {
    if(field==0)return t[index].line-(index?t[index-1].line:0);
    if(field==1)return t[index].column-(index&&t[index].line==t[index-1].line?t[index-1].column:0);
    return field==2?t[index].length:field==3?t[index].type:t[index].modifiers;
}
static XrXirStatus tokens_equal_encoded(XrCompileResources *resources,const XlspSourceToken *a,size_t ai,
    const XlspSourceToken *b,size_t bi,bool *equal) {
    if(xr_compile_resources_work(resources,10)!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
    *equal=true;for(unsigned f=0;f<5;++f)if(tokens_word(a,ai,f)!=tokens_word(b,bi,f)){*equal=false;break;}return XR_XIR_OK;
}
static bool tokens_before(uint32_t line,uint32_t column,XrLspPosition p) {
    return line<p.line||(line==p.line&&column<p.character);
}
XrXirStatus xlsp_source_tokens_json(XrCompileResources *resources,const XlspSourceTokens *fresh,
    const XlspSourceTokens *previous,uint32_t result_id,bool delta,const XrLspRange *range,XrJsonValue **output) {
    if(!resources||!fresh||!output||*output||(!range&&!result_id)||(range&&delta))return XR_XIR_BAD_STRUCTURE;
    if(range&&(range->start.line>range->end.line||(range->start.line==range->end.line&&range->start.character>range->end.character)))return XR_XIR_BAD_STRUCTURE;
    size_t count=0,old_count=0;const XlspSourceToken *items=xlsp_source_tokens_items(fresh,&count);
    const XlspSourceToken *old=xlsp_source_tokens_items(previous,&old_count);
    if(count>INT32_MAX/5||old_count>INT32_MAX/5)return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;XrJsonValue *result=xjson_new_object(),*array=NULL,*edit=NULL;
    if(!result)return XR_XIR_OUT_OF_MEMORY;
    if(!range) {
        char id[16];int length=snprintf(id,sizeof(id),"%u",result_id);
        if(length<=0||(size_t)length>=sizeof(id)){status=XR_XIR_BAD_STRUCTURE;goto done;}
        if(!navigation_member(result,"resultId",xjson_new_string(id))){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    }
    array=xjson_new_array();if(!array){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    if(delta&&previous) {
        size_t prefix=0,suffix=0;bool equal=false;
        while(prefix<count&&prefix<old_count) {
            status=tokens_equal_encoded(resources,items,prefix,old,prefix,&equal);if(status!=XR_XIR_OK)goto done;
            if(!equal)break;++prefix;
        }
        while(suffix<count-prefix&&suffix<old_count-prefix) {
            status=tokens_equal_encoded(resources,items,count-suffix-1,old,old_count-suffix-1,&equal);if(status!=XR_XIR_OK)goto done;
            if(!equal)break;++suffix;
        }
        if(prefix+suffix<count||prefix+suffix<old_count) {
            edit=xjson_new_object();if(!edit){status=XR_XIR_OUT_OF_MEMORY;goto done;}
            if(!navigation_member(edit,"start",xjson_new_number((double)(prefix*5)))||
                !navigation_member(edit,"deleteCount",xjson_new_number((double)((old_count-prefix-suffix)*5)))){status=XR_XIR_OUT_OF_MEMORY;goto done;}
            if(prefix+suffix<count) {
                XrJsonValue *insert=xjson_new_array();if(!insert){status=XR_XIR_OUT_OF_MEMORY;goto done;}
                for(size_t i=prefix;i<count-suffix&&status==XR_XIR_OK;++i)for(unsigned f=0;f<5;++f) {
                    if(xr_compile_resources_work(resources,1)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;break;}
                    status=tokens_array_push(insert,xjson_new_number(tokens_word(items,i,f)));if(status!=XR_XIR_OK)break;
                }
                if(status!=XR_XIR_OK){xjson_free(insert);goto done;}
                if(!navigation_member(edit,"data",insert)){status=XR_XIR_OUT_OF_MEMORY;goto done;}
            }
            XrJsonValue *transferred=edit;edit=NULL;
            status=tokens_array_push(array,transferred);if(status!=XR_XIR_OK)goto done;
        }
        {XrJsonValue *transferred=array;array=NULL;
        if(!navigation_member(result,"edits",transferred)){status=XR_XIR_OUT_OF_MEMORY;goto done;}}
    } else {
        uint32_t last_line=0,last_column=0;
        for(size_t i=0;i<count;++i) {
            if(xr_compile_resources_work(resources,sizeof(*items))!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
            const XlspSourceToken *t=&items[i];
            if(range&&range->start.line==range->end.line&&range->start.character==range->end.character)continue;
            if(range&&(!tokens_before(t->line,t->column,range->end)||!tokens_before(range->start.line,range->start.character,(XrLspPosition){t->line,t->column+t->length})))continue;
            uint32_t encoded[5]={t->line-last_line,t->column-(t->line==last_line?last_column:0),t->length,t->type,t->modifiers};
            for(unsigned f=0;f<5;++f){status=tokens_array_push(array,xjson_new_number(encoded[f]));if(status!=XR_XIR_OK)goto done;}
            last_line=t->line;last_column=t->column;
        }
        {XrJsonValue *transferred=array;array=NULL;
        if(!navigation_member(result,"data",transferred)){status=XR_XIR_OUT_OF_MEMORY;goto done;}}
    }
    *output=result;result=NULL;
done:
    xjson_free(edit);xjson_free(array);xjson_free(result);return status;
}
