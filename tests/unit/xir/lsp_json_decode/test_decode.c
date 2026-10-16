/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#include "../lsp_json_clone/test_allocator.h"
#include "base/xjson_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define CHECK(c) do{if(!(c)){fprintf(stderr,"decode:%d:%s\n",__LINE__,#c);exit(2);}}while(0)
static void zero(void) {CloneStats stats=clone_stats();CHECK(!stats.live&&!stats.bytes);}
#ifndef DECODE_BEFORE
static void facts(const XrJsonValue *value,unsigned mode) {
    CHECK(value);
    if(mode==0)CHECK(value->type==XR_JSON_NULL);
    if(mode==1)CHECK(value->type==XR_JSON_BOOL&&value->as.boolean);
    if(mode==2)CHECK(value->type==XR_JSON_BOOL&&!value->as.boolean);
    if(mode==3)CHECK(value->type==XR_JSON_NUMBER&&value->is_integer&&value->as.integer==INT64_MAX);
    if(mode==4)CHECK(value->type==XR_JSON_NUMBER&&!value->is_integer&&value->as.number==-12500);
    if(mode==5)CHECK(value->type==XR_JSON_STRING&&value->string_len==7&&!memcmp(value->as.string,"a\0b\xf0\x9f\x98\x80",7));
    if(mode==6){CHECK(value->type==XR_JSON_ARRAY&&value->as.array.count==20);for(int i=0;i<20;++i)CHECK(value->as.array.items[i]->as.integer==i);}
    if(mode==7){CHECK(value->type==XR_JSON_OBJECT&&value->as.object.count==20);
        for(int i=0;i<20;++i){const XrJsonMember *m=&value->as.object.members[i];CHECK(m->key_len==3&&!memcmp(m->key,"a\0b",3)&&m->value->as.integer==i);}}
    if(mode==8){CHECK(value->type==XR_JSON_STRING&&value->string_len==1024);for(size_t i=0;i<1024;++i)CHECK(value->as.string[i]=='x');}
    if(mode==9)CHECK(value->type==XR_JSON_NUMBER&&!value->is_integer&&value->as.number>1e70);
    if(mode==10){for(unsigned i=1;i<256;++i){CHECK(value->type==XR_JSON_ARRAY&&value->as.array.count==1);value=value->as.array.items[0];}
        CHECK(value->type==XR_JSON_ARRAY&&!value->as.array.count);}
}
static size_t valid_case(const char *text,size_t length,unsigned mode,bool full) {
    char *producer=malloc(length);CHECK(producer);memcpy(producer,text,length);
    clone_fault(SIZE_MAX);XrJsonValue *value=NULL;CHECK(xjson_parse_status(producer,length,&value)==XR_JSON_PARSE_OK);
    size_t sites=clone_stats().attempts;CHECK(sites);memset(producer,'?',length);free(producer);facts(value,mode);xjson_free(value);zero();
    clone_fault(SIZE_MAX);value=xjson_parse(text,length);facts(value,mode);xjson_free(value);zero();
    size_t checked=full?sites:1;
    for(size_t i=0;i<checked;++i){clone_fault(i);value=NULL;CHECK(xjson_parse_status(text,length,&value)==XR_JSON_PARSE_OUT_OF_MEMORY&&!value);
        CloneStats stats=clone_stats();CHECK(stats.injected&&stats.attempts==i+1);zero();}
    clone_fault(SIZE_MAX);printf("{\"case\":%u,\"sites\":%zu,\"checked\":%zu,\"producer_dead\":true,\"physical0_0\":true}\n",mode,sites,checked);return sites;
}
static size_t valid_cases(bool full) {
    const char *texts[]={"null","true","false","9223372036854775807","-12.5e3","\"a\\u0000b\\ud83d\\ude00\""};
    size_t total=0;for(unsigned i=0;i<6;++i)total+=valid_case(texts[i],strlen(texts[i]),i,full);
    char array[256]="[",object[512]="{";
    for(unsigned i=0;i<20;++i){size_t a=strlen(array),b=strlen(object);snprintf(array+a,sizeof(array)-a,"%s%u",i?",":"",i);
        snprintf(object+b,sizeof(object)-b,"%s\"a\\u0000b\":%u",i?",":"",i);}
    strcat(array,"]");strcat(object,"}");total+=valid_case(array,strlen(array),6,full);total+=valid_case(object,strlen(object),7,full);
    char string[1026];string[0]=string[1025]='"';memset(string+1,'x',1024);total+=valid_case(string,sizeof(string),8,full);
    char number[80];memset(number,'1',sizeof(number));total+=valid_case(number,sizeof(number),9,full);
    char depth[512];memset(depth,'[',256);memset(depth+256,']',256);total+=valid_case(depth,sizeof(depth),10,full);return total;
}
static size_t invalid_case(const char *text,size_t length,bool full) {
    clone_fault(SIZE_MAX);XrJsonValue *out=NULL;CHECK(xjson_parse_status(text,length,&out)==XR_JSON_PARSE_INVALID_JSON&&!out);
    size_t sites=clone_stats().attempts;zero();size_t checked=full?sites:sites?1:0;
    for(size_t i=0;i<checked;++i){clone_fault(i);CHECK(xjson_parse_status(text,length,&out)==XR_JSON_PARSE_OUT_OF_MEMORY&&!out);CHECK(clone_stats().injected);zero();}
    clone_fault(SIZE_MAX);return sites;
}
static size_t invalid_cases(bool full) {
    const char *texts[]={""," ","n","nul","nullx","t","tru","fals","-","01","1.","1e","1e+","[1,]","{\"x\":}","{\"x\" 1}","{\"x\":1,}","\"\\x\"","\"\\uD800\"","\"\\uDC00\"","\"\\uD800\\u0041\"","\"\\u123","{}[]"};
    size_t total=0;for(size_t i=0;i<sizeof(texts)/sizeof(texts[0]);++i)total+=invalid_case(texts[i],strlen(texts[i]),full);
    for(unsigned byte=0;byte<32;++byte){char text[3]={'"',(char)byte,'"'};total+=invalid_case(text,sizeof(text),full);}
    const char *bad[]={"\x80","\xc0\x80","\xe0\x80\x80","\xed\xa0\x80","\xf0\x80\x80\x80","\xf4\x90\x80\x80","\xc2","\xe2\x82","\xf0\x9f\x98"};
    for(size_t i=0;i<9;++i){char text[32];int size=snprintf(text,sizeof(text),"\"%s\"",bad[i]);CHECK(size>0);total+=invalid_case(text,(size_t)size,full);
        size=snprintf(text,sizeof(text),"{\"%s\":0}",bad[i]);CHECK(size>0);total+=invalid_case(text,(size_t)size,full);}
    char depth[514];memset(depth,'[',257);memset(depth+257,']',257);total+=invalid_case(depth,sizeof(depth),full);
    puts("Decode invalid=74 grammar23 control32 UTF8_value_and_key18 depth1 physical0/0");return total;
}
static void arguments(void) {
    XrJsonValue *out=NULL,*occupied=xjson_new_null();CHECK(occupied);XrJsonValue *saved=occupied;clone_fault(SIZE_MAX);
    CHECK(xjson_parse_status(NULL,0,&out)==XR_JSON_PARSE_BAD_ARGUMENT&&!out);
    CHECK(xjson_parse_status("null",4,NULL)==XR_JSON_PARSE_BAD_ARGUMENT);
    CHECK(xjson_parse_status("null",4,&occupied)==XR_JSON_PARSE_BAD_ARGUMENT&&occupied==saved);
    CHECK(xjson_parse_status("n",(size_t)PTRDIFF_MAX+1,&out)==XR_JSON_PARSE_BAD_ARGUMENT&&!out);
    CHECK(!clone_stats().attempts);xjson_free(occupied);zero();puts("Decode bad_arguments=4 untouched_output=1 allocation0");
}
#else
static int before(const char *mode) {
    if(!strcmp(mode,"--array-before")){clone_fault(2);XrJsonValue *value=xjson_parse("[1]",3);
        CHECK(value&&value->type==XR_JSON_ARRAY&&!value->as.array.count&&clone_stats().injected);xjson_free(value);
        CHECK(clone_stats().live==1);size_t leaked=clone_forced_cleanup();CHECK(leaked==1);zero();
        puts("BUG decode-array partial=1 count=0 leaked=1 raw_cleanup_only=1");return 1;}
    if(!strcmp(mode,"--status-before")){clone_fault(0);CHECK(!xjson_parse("{}",2)&&clone_stats().injected);zero();
        clone_fault(SIZE_MAX);CHECK(!xjson_parse("{",1)&&!clone_stats().injected);zero();puts("BUG decode-status OOM_and_invalid_NULL=1 physical0/0");return 1;}
    CHECK(!strcmp(mode,"--unicode-before"));const unsigned char raw[2][3]={{'"',255,'"'},{'"','\n','"'}};
    for(unsigned i=0;i<2;++i){clone_fault(SIZE_MAX);XrJsonValue *value=xjson_parse((const char *)raw[i],3);CHECK(value);xjson_free(value);zero();}
    puts("BUG decode-unicode raw_invalid_accepted=2 physical0/0");return 1;
}
#endif
int main(int argc,char **argv) {
#ifdef DECODE_BEFORE
    CHECK(argc==2);return before(argv[1]);
#else
    CHECK(argc==2);bool full=!strcmp(argv[1],"--resources");CHECK(full||!strcmp(argv[1],"--normal"));arguments();
    size_t valid=valid_cases(full),invalid=invalid_cases(full);zero();
    printf("Decode valid11 invalid74 sites=%zu invalid_sites=%zu full_FI=%u physical0/0\n",valid,invalid,full?1u:0u);return 0;
#endif
}
