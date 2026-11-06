/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#ifndef XLSP_INDEX_JSON_H
#define XLSP_INDEX_JSON_H
#include "xlsp_source_index.h"
#include "../../base/xjson.h"
#include <string.h>
static inline bool index_json_member(XrJsonValue *object,const char *key,XrJsonValue *child) {
    if(!child)return false;
    int before=object->as.object.count;xjson_object_set_new(object,key,child);
    if(object->as.object.count==before){xjson_free(child);return false;}
    if(object->as.object.count!=before+1)return false;
    XrJsonMember *member=&object->as.object.members[before];
    return member->key&&member->value==child&&member->key_len==strlen(key)&&!memcmp(member->key,key,member->key_len);
}
static inline XrJsonValue *index_json_position(XrLspPosition position) {
    XrJsonValue *value=xjson_new_object();if(!value)return NULL;
    if(!index_json_member(value,"line",xjson_new_number(position.line))||
       !index_json_member(value,"character",xjson_new_number(position.character))){xjson_free(value);return NULL;}
    return value;
}
static inline XrJsonValue *index_json_range(XrLspRange range) {
    XrJsonValue *value=xjson_new_object();if(!value)return NULL;
    if(!index_json_member(value,"start",index_json_position(range.start))||
       !index_json_member(value,"end",index_json_position(range.end))){xjson_free(value);return NULL;}
    return value;
}
static inline XrXirStatus index_json_contains(const char *name,const char *query,size_t length,bool *out) {
    if(!length){*out=true;return XR_XIR_OK;}
    for(size_t i=0;;++i) {
        if(!name[i]){*out=false;return XR_XIR_OK;}
        size_t j=0;
        for(;j<length;++j) {
            unsigned char c=(unsigned char)name[i+j];if(!c||c!=(unsigned char)query[j])break;
        }
        if(j==length){*out=true;return XR_XIR_OK;}
    }
}
#endif // XLSP_INDEX_JSON_H
