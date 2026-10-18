/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "xlsp_source_code_lens.h"
#include "xlsp_semantic_legend.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>

static bool lens_member(XrJsonValue *object,const char *key,XrJsonValue *value) {
    if(!value)return false;
    int before=object->as.object.count;xjson_object_set_new(object,key,value);
    if(object->as.object.count==before){xjson_free(value);return false;}
    if(object->as.object.count!=before+1)return false;
    XrJsonMember *member=&object->as.object.members[before];
    return member->key&&member->value==value&&member->key_len==strlen(key)&&
        !memcmp(member->key,key,member->key_len);
}
static XrJsonValue *lens_position(uint32_t line,uint32_t column) {
    XrJsonValue *value=xjson_new_object();if(!value)return NULL;
    if(!lens_member(value,"line",xjson_new_number(line))||
       !lens_member(value,"character",xjson_new_number(column))) {
        xjson_free(value);return NULL;
    }
    return value;
}
static XrJsonValue *lens_range(const XlspSourceToken *token) {
    XrJsonValue *value=xjson_new_object();if(!value)return NULL;
    if(!lens_member(value,"start",lens_position(token->line,token->column))||
       !lens_member(value,"end",lens_position(token->line,token->column+token->length))) {
        xjson_free(value);return NULL;
    }
    return value;
}
static XrJsonValue *lens_value(const XlspSourceToken *token,int references) {
    char title[64];int length=snprintf(title,sizeof(title),"%d reference%s",
        references,references==1?"":"s");
    if(length<=0||(size_t)length>=sizeof(title))return NULL;
    XrJsonValue *lens=xjson_new_object(),*command=NULL;
    if(!lens)return NULL;
    if(!lens_member(lens,"range",lens_range(token)))goto failed;
    command=xjson_new_object();if(!command)goto failed;
    if(!lens_member(command,"title",xjson_new_string(title))||
       !lens_member(command,"command",xjson_new_string("")))goto failed;
    {XrJsonValue *moved=command;command=NULL;if(!lens_member(lens,"command",moved))goto failed;}
    return lens;
failed:
    xjson_free(command);xjson_free(lens);return NULL;
}
XR_FUNC XrXirStatus xlsp_source_code_lens_json(XrCompileResources *resources,
    XlspSourceWorkspace *workspace,const char *uri,size_t length,XrJsonValue **out) {
    if(!resources||!workspace||!uri||length==SIZE_MAX||!out||*out)return XR_XIR_BAD_STRUCTURE;
    XlspSourceTokens *tokens=NULL;XrJsonValue *lenses=NULL,*references=NULL;
    XrXirStatus status=xlsp_source_workspace_tokens(workspace,uri,length,NULL,&tokens);
    if(status!=XR_XIR_OK)goto done;
    lenses=xjson_new_array();if(!lenses){status=XR_XIR_OUT_OF_MEMORY;goto done;}
    size_t count=0;const XlspSourceToken *items=xlsp_source_tokens_items(tokens,&count);
    for(size_t i=0;i<count;++i) {
        if(xr_compile_resources_work(resources,1)!=XR_COMPILE_RESOURCE_OK){status=XR_XIR_BUDGET;goto done;}
        const XlspSourceToken *token=&items[i];
        if(!(token->modifiers&XLSP_MOD_DECLARATION)||
           (token->type!=XLSP_TOKEN_FUNCTION&&token->type!=XLSP_TOKEN_METHOD&&
            token->type!=XLSP_TOKEN_CLASS&&token->type!=XLSP_TOKEN_STRUCT))continue;
        if(!token->length||token->line>INT32_MAX||token->column>INT32_MAX||
           token->length>INT32_MAX-token->column){status=XR_XIR_BAD_STRUCTURE;goto done;}
        XrXirSourceDeclarationKind direct=XR_XIR_SOURCE_FUNCTION;
        status=xlsp_source_workspace_declaration_kind(workspace,uri,length,
            (XrLspPosition){token->line,token->column},&direct);
        if(status!=XR_XIR_OK)goto done;
        if(direct==XR_XIR_SOURCE_IMPORT||direct==XR_XIR_SOURCE_MODULE)continue;
        status=xlsp_source_workspace_navigation(workspace,uri,length,
            (XrLspPosition){token->line,token->column},1,false,&references);
        if(status!=XR_XIR_OK)goto done;
        if(!references||references->type!=XR_JSON_ARRAY||references->as.array.count<0) {
            status=XR_XIR_BAD_STRUCTURE;goto done;
        }
        int reference_count=references->as.array.count;xjson_free(references);references=NULL;
        if(lenses->as.array.count==INT_MAX||xr_compile_resources_work(resources,128)!=XR_COMPILE_RESOURCE_OK) {
            status=XR_XIR_BUDGET;goto done;
        }
        XrJsonValue *lens=lens_value(token,reference_count);
        if(!lens){status=XR_XIR_OUT_OF_MEMORY;goto done;}
        int before=lenses->as.array.count;xjson_array_push(lenses,lens);
        if(lenses->as.array.count==before){xjson_free(lens);status=XR_XIR_OUT_OF_MEMORY;goto done;}
    }
    *out=lenses;lenses=NULL;
done:
    xjson_free(references);xjson_free(lenses);xlsp_source_tokens_free(tokens);return status;
}
