/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_semantic_tokens.h"
#include <string.h>

// Token type names for legend
static const char *token_type_names[] = {
    "namespace",     "type",      "class",    "enum",     "interface",  "struct",
    "typeParameter", "parameter", "variable", "property", "enumMember", "event",
    "function",      "method",    "macro",    "keyword",  "modifier",   "comment",
    "string",        "number",    "regexp",   "operator"};

// Token modifier names for legend
static const char *token_modifier_names[] = {"declaration",  "definition",    "readonly",
                                             "static",       "deprecated",    "async",
                                             "modification", "documentation", "defaultLibrary"};


static bool semantic_member(XrJsonValue *object,const char *key,XrJsonValue *value) {
    if(!value)return false;
    int before=object->as.object.count;xjson_object_set_new(object,key,value);
    if(object->as.object.count==before){xjson_free(value);return false;}
    if(object->as.object.count!=before+1)return false;
    XrJsonMember *member=&object->as.object.members[before];
    return member->key&&member->value==value&&member->key_len==strlen(key)&&!memcmp(member->key,key,member->key_len);
}
static XrJsonValue *semantic_names(const char *const *names,size_t count) {
    XrJsonValue *array=xjson_new_array();if(!array)return NULL;
    for(size_t i=0;i<count;++i) {
        XrJsonValue *value=xjson_new_string(names[i]);if(!value){xjson_free(array);return NULL;}
        int before=array->as.array.count;xjson_array_push(array,value);
        if(array->as.array.count==before){xjson_free(value);xjson_free(array);return NULL;}
    }
    return array;
}
/* Capability construction is a transaction in the protocol allocation domain.
 * Both pre-adoption capacity failure and post-adoption key failure reject it. */
XR_FUNC XrJsonValue *xlsp_semantic_tokens_provider(void) {
    XrJsonValue *provider=NULL,*legend=xjson_new_object(),*full=NULL;
    if(!legend)return NULL;
    if(!semantic_member(legend,"tokenTypes",semantic_names(token_type_names,XLSP_TOKEN_COUNT))||
       !semantic_member(legend,"tokenModifiers",semantic_names(token_modifier_names,
           sizeof(token_modifier_names)/sizeof(*token_modifier_names))))goto failed;
    full=xjson_new_object();if(!full||!semantic_member(full,"delta",xjson_new_bool(true)))goto failed;
    provider=xjson_new_object();if(!provider)goto failed;
    {XrJsonValue *moved=legend;legend=NULL;if(!semantic_member(provider,"legend",moved))goto failed;}
    {XrJsonValue *moved=full;full=NULL;if(!semantic_member(provider,"full",moved))goto failed;}
    if(!semantic_member(provider,"range",xjson_new_bool(true)))goto failed;
    return provider;
failed:
    xjson_free(provider);xjson_free(legend);xjson_free(full);return NULL;
}
