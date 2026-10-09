/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_source_navigation.h"
#include <limits.h>
#include <string.h>

/* xjson's mutation API is void. A new-key operation may fail either before
 * adoption (capacity allocation) or after adoption (key copy). Observe both
 * real publication boundaries, and free each child exactly once. */
static bool navigation_member(XrJsonValue *object,const char *key,XrJsonValue *child) {
    if(!child)return false;
    int before=object->as.object.count;
    xjson_object_set_new(object,key,child);
    if(object->as.object.count==before) { xjson_free(child);return false; }
    if(object->as.object.count!=before+1)return false;
    const XrJsonMember *member=&object->as.object.members[before];
    return member->key && member->value==child && member->key_len==strlen(key) &&
        !memcmp(member->key,key,member->key_len);
}
static XrJsonValue *navigation_position(XrLspPosition position) {
    XrJsonValue *object=xjson_new_object();
    if(!object)return NULL;
    if(!navigation_member(object,"line",xjson_new_number((double)position.line)) ||
        !navigation_member(object,"character",xjson_new_number((double)position.character))) {
        xjson_free(object);return NULL;
    }
    return object;
}
static XrJsonValue *navigation_range(XrLspRange range) {
    XrJsonValue *object=xjson_new_object();
    if(!object)return NULL;
    if(!navigation_member(object,"start",navigation_position(range.start)) ||
        !navigation_member(object,"end",navigation_position(range.end))) {
        xjson_free(object);return NULL;
    }
    return object;
}
static XrJsonValue *navigation_location(const XlspSourceLocation *location) {
    XrJsonValue *object=xjson_new_object();
    if(!object)return NULL;
    if(!navigation_member(object,"uri",xjson_new_string(location->uri)) ||
        !navigation_member(object,"range",navigation_range(location->range))) {
        xjson_free(object);return NULL;
    }
    return object;
}
XrXirStatus xlsp_source_definition_json(const XlspSourceSnapshot *snapshot,
    const char *uri,size_t length,XrLspPosition position,XrJsonValue **output) {
    if(!output||*output)return XR_XIR_BAD_STRUCTURE;
    XlspSourceLocation location={0};
    XrXirStatus status=xlsp_source_definition(snapshot,uri,length,position,&location);
    if(status!=XR_XIR_OK)return status;
    XrJsonValue *result=navigation_location(&location);
    if(!result)return XR_XIR_OUT_OF_MEMORY;
    *output=result;return XR_XIR_OK;
}
XrXirStatus xlsp_source_references_json(const XlspSourceSnapshot *snapshot,
    const char *uri,size_t length,XrLspPosition position,bool include_declaration,XrJsonValue **output) {
    if(!output||*output)return XR_XIR_BAD_STRUCTURE;
    XlspSourceLocations locations={0};XrJsonValue *result=NULL;
    XrXirStatus status=xlsp_source_references(snapshot,uri,length,position,include_declaration,&locations);
    if(status!=XR_XIR_OK)return status;
    result=xjson_new_array();
    if(!result) { status=XR_XIR_OUT_OF_MEMORY;goto done; }
    for(size_t i=0;i<locations.count;++i) {
        /* Prevent signed growth overflow in the generic void array builder. */
        if(result->as.array.count==result->as.array.capacity && result->as.array.capacity>INT_MAX/2) {
            status=XR_XIR_BUDGET;goto done;
        }
        XrJsonValue *item=navigation_location(&locations.items[i]);
        if(!item) { status=XR_XIR_OUT_OF_MEMORY;goto done; }
        int before=result->as.array.count;xjson_array_push(result,item);
        if(result->as.array.count!=before+1) {
            xjson_free(item);status=XR_XIR_OUT_OF_MEMORY;goto done;
        }
    }
    *output=result;result=NULL;
done:
    xjson_free(result);xlsp_source_locations_free(&locations);return status;
}

static bool navigation_same_range(XrLspRange a,XrLspRange b) {
    return a.start.line==b.start.line&&a.start.character==b.start.character&&
        a.end.line==b.end.line&&a.end.character==b.end.character;
}
static XrXirStatus navigation_string_equal(XrCompileResources *r,const char *a,const char *b,bool *out) {
    for(;;++a,++b) {
        XrCompileResourceStatus paid=xr_compile_resources_work(r,2);
        if(paid!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
        if(*a!=*b){*out=false;return XR_XIR_OK;}
        if(!*a){*out=true;return XR_XIR_OK;}
    }
}
static bool navigation_json_range(XrJsonValue *value,XrLspRange range) {
    XrJsonValue *r=xjson_get_object(value,"range");
    XrJsonValue *a=xjson_get_object(r,"start"),*b=xjson_get_object(r,"end");
    return (uint64_t)xjson_get_int(a,"line")==range.start.line&&
        (uint64_t)xjson_get_int(a,"character")==range.start.character&&
        (uint64_t)xjson_get_int(b,"line")==range.end.line&&
        (uint64_t)xjson_get_int(b,"character")==range.end.character;
}
static XrXirStatus navigation_uri_equal(XrCompileResources *r,const char *a,const char *uri,size_t length,bool *out) {
    for(size_t i=0;i<length;++i) {
        if(xr_compile_resources_work(r,2)!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
        if(!a[i]||a[i]!=uri[i]){*out=false;return XR_XIR_OK;}
    }
    if(xr_compile_resources_work(r,1)!=XR_COMPILE_RESOURCE_OK)return XR_XIR_BUDGET;
    *out=a[length]==0;return XR_XIR_OK;
}
XrXirStatus xlsp_source_navigation_many_json(XrCompileResources *resources,
    XlspSourceSnapshot *const *snapshots,size_t count,const char *uri,size_t length,
    XrLspPosition position,unsigned mode,bool include_declaration,XrJsonValue **output) {
    if(!resources||!snapshots||!count||!uri||length==SIZE_MAX||mode>2||!output||*output)return XR_XIR_BAD_STRUCTURE;
    XrJsonValue *result=NULL;XlspSourceLocations locations={0};
    XlspSourceLocation definition={0};bool found=false;XrXirStatus status=XR_XIR_OK;
    if(mode){result=xjson_new_array();if(!result)return XR_XIR_OUT_OF_MEMORY;}
    for(size_t i=0;i<count;++i) {
        bool contains=false;status=xlsp_source_snapshot_contains(snapshots[i],uri,length,&contains);
        if(status!=XR_XIR_OK)goto done;
        if(!contains)continue;
        XlspSourceLocation current={0};
        status=xlsp_source_definition(snapshots[i],uri,length,position,&current);
        if(status!=XR_XIR_OK)goto done;
        if(found) {
            if(xr_compile_resources_work(resources,10)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
            bool equal=false;status=navigation_string_equal(resources,definition.uri,current.uri,&equal);
            if(status!=XR_XIR_OK)goto done;
            if(!equal||!navigation_same_range(definition.range,current.range)||definition.version!=current.version) {
                status=XR_XIR_BAD_STRUCTURE;goto done;
            }
        } else {definition=current;found=true;}
        if(!mode)continue;
        status=xlsp_source_references(snapshots[i],uri,length,position,mode==2||include_declaration,&locations);
        if(status!=XR_XIR_OK)goto done;
        for(size_t j=0;j<locations.count;++j) {
            const XlspSourceLocation *location=&locations.items[j];bool equal=false;
            if(mode==2) {
                status=navigation_uri_equal(resources,location->uri,uri,length,&equal);
                if(status!=XR_XIR_OK)goto done;
                if(!equal)continue;
            }
            bool duplicate=false;
            for(int k=0;k<result->as.array.count;++k) {
                if(xr_compile_resources_work(resources,8)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
                XrJsonValue *item=result->as.array.items[k];
                if(!navigation_json_range(item,location->range))continue;
                equal=mode==2;
                if(!equal) {
                    status=navigation_string_equal(resources,xjson_get_string(item,"uri"),location->uri,&equal);
                    if(status!=XR_XIR_OK)goto done;
                }
                if(equal){duplicate=true;break;}
            }
            if(duplicate)continue;
            XrJsonValue *item;
            if(mode==2) {
                item=xjson_new_object();
                int kind=location->access==XR_XIR_SOURCE_WRITE||location->access==XR_XIR_SOURCE_READ_WRITE?3:2;
                if(item&&(!navigation_member(item,"range",navigation_range(location->range))||
                    !navigation_member(item,"kind",xjson_new_number(kind)))){xjson_free(item);item=NULL;}
            } else item=navigation_location(location);
            if(!item){status=XR_XIR_OUT_OF_MEMORY;goto done;}
            if(result->as.array.count==result->as.array.capacity&&result->as.array.capacity>INT_MAX/2) {
                xjson_free(item);status=XR_XIR_BUDGET;goto done;
            }
            int before=result->as.array.count;xjson_array_push(result,item);
            if(result->as.array.count!=before+1){xjson_free(item);status=XR_XIR_OUT_OF_MEMORY;goto done;}
        }
        xlsp_source_locations_free(&locations);
    }
    if(!found){status=XR_XIR_UNRESOLVED;goto done;}
    if(!mode){result=navigation_location(&definition);if(!result){status=XR_XIR_OUT_OF_MEMORY;goto done;}}
    *output=result;result=NULL;
done:
    xlsp_source_locations_free(&locations);xjson_free(result);return status;
}

#include "xlsp_source_hover_json_impl.h"

#include "xlsp_source_tokens_json_impl.h"
