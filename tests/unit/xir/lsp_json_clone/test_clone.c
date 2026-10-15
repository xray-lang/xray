/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu. MIT License.
 *
 * test_clone.c - Exact JSON values and failure-atomic clone ownership
 */
#include "test_allocator.h"
#include "base/xjson.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#define CHECK(c) do {if(!(c)){fprintf(stderr,"%d:%s\n",__LINE__,#c);exit(2);}}while(0)
static void zero(void) {CloneStats s=clone_stats();CHECK(!s.live&&!s.bytes);}
static XrJsonValue *parse(const char *text) {XrJsonValue *v=xjson_parse(text,strlen(text));CHECK(v);return v;}
static char *encoded(XrJsonValue *value) {size_t length=0;char *s=xjson_stringify(value,&length);CHECK(s&&strlen(s)==length);return s;}
static const char *cases[]={
    "null","9223372036854775807",
    "[null,true,false,-0,1.0,1.25,\"x\\u0000y\",[1,2],{\"k\":\"v\"}]",
    "{\"a\\u0000b\":1,\"dup\":1,\"dup\":2,\"\":3,\"unicode\":\"\\ud83d\\ude00\"}",
    "{}","[]","\"\""
};
static void facts(XrJsonValue *value,unsigned mode) {
    if(mode==1)CHECK(value->type==XR_JSON_NUMBER&&value->is_integer&&value->as.integer==INT64_MAX);
    if(mode==2){CHECK(value->as.array.count==9);XrJsonValue *floating=xjson_array_get(value,4),*string=xjson_array_get(value,6);
        CHECK(!floating->is_integer&&floating->as.number==1.0&&string->string_len==3&&!memcmp(string->as.string,"x\0y",3));}
    if(mode==3){CHECK(value->as.object.count==5);const XrJsonMember *m=value->as.object.members;
        CHECK(m[0].key_len==3&&!memcmp(m[0].key,"a\0b",3)&&m[1].key_len==3&&m[2].key_len==3);
        CHECK(!memcmp(m[1].key,"dup",3)&&!memcmp(m[2].key,"dup",3)&&m[1].value->as.integer==1&&m[2].value->as.integer==2);}
}
static size_t clone_case(const char *text,unsigned mode,bool full) {
    clone_fault(SIZE_MAX);XrJsonValue *input=parse(text);char *expected=encoded(input);CloneStats baseline=clone_stats();
    clone_fault(SIZE_MAX);XrJsonValue *copy=xjson_clone(input);CHECK(copy);size_t sites=clone_stats().attempts;CHECK(sites);
    xjson_free(copy);CHECK(clone_stats().live==baseline.live&&clone_stats().bytes==baseline.bytes);
    if(full)for(size_t prefix=0;prefix<sites;++prefix){clone_fault(prefix);copy=xjson_clone(input);CloneStats failed=clone_stats();
        CHECK(failed.injected&&failed.attempts==prefix+1&&!copy&&failed.live==baseline.live&&failed.bytes==baseline.bytes);}
    clone_fault(SIZE_MAX);char *unchanged=encoded(input);CHECK(!strcmp(unchanged,expected));clone_test_free(unchanged);
    copy=xjson_clone(input);CHECK(copy);xjson_free(input);facts(copy,mode);char *actual=encoded(copy);
    CHECK(!strcmp(expected,actual));clone_test_free(expected);clone_test_free(actual);xjson_free(copy);zero();
    printf("{\"case\":%u,\"sites\":%zu,\"full\":%s,\"producer_dead\":true,\"physical_live\":0,\"physical_bytes\":0}\n",mode,sites,full?"true":"false");
    return full?sites:0;
}
static void append_call(XrJsonValue *container,XrJsonValue *value,unsigned mode) {
    if(mode==0)xjson_array_push(container,value);
    else if(mode==1)xjson_object_set(container,"key",value);
    else xjson_object_set_new(container,"key",value);
}
static void append_cases(void) {
    for(unsigned mode=0;mode<3;++mode){size_t sites=mode?2:1;
        for(size_t prefix=0;prefix<sites;++prefix){clone_fault(SIZE_MAX);XrJsonValue *container=mode?xjson_new_object():xjson_new_array(),*value=xjson_new_string("child");CHECK(container&&value);
            CloneStats before=clone_stats();clone_fault(prefix);append_call(container,value,mode);CloneStats after=clone_stats();
            CHECK(after.injected&&after.attempts==prefix+1&&after.live==before.live&&after.bytes==before.bytes);
            if(mode)CHECK(!container->as.object.count&&!container->as.object.capacity&&!container->as.object.members);
            else CHECK(!container->as.array.count&&!container->as.array.capacity&&!container->as.array.items);
            clone_fault(SIZE_MAX);xjson_free(value);xjson_free(container);zero();
        }
    }
    clone_fault(SIZE_MAX);XrJsonValue *object=parse("{\"a\\u0000b\":1}"),*value=xjson_new_string("owned");CHECK(value);
    xjson_object_set(object,"a",value);CHECK(object->as.object.count==2&&object->as.object.members[0].value->as.integer==1);
    CloneStats before=clone_stats();clone_fault(0);xjson_object_set(object,"a",value);
    CHECK(!clone_stats().attempts&&clone_stats().live==before.live&&clone_stats().bytes==before.bytes);
    clone_fault(SIZE_MAX);xjson_free(object);zero();puts("Append rollback5 prefixes, exact key matching and same-value ownership PASS");
}
#include "clone_boundaries.h"
static int probe_key(void) {
    clone_fault(SIZE_MAX);XrJsonValue *input=parse("{\"a\\u0000b\":1}"),*copy=xjson_clone(input);CHECK(copy);
    size_t expected=input->as.object.members[0].key_len,actual=copy->as.object.members[0].key_len;
    xjson_free(input);xjson_free(copy);zero();
    if(actual!=expected){printf("BUG key-bytes expected=%zu actual=%zu\n",expected,actual);return 1;}return 0;
}
static int probe_clone(void) {
    clone_fault(SIZE_MAX);XrJsonValue *input=parse("[1,2]");CloneStats before=clone_stats();
    clone_fault(2);XrJsonValue *copy=xjson_clone(input);CloneStats after=clone_stats();bool partial=copy!=NULL;
    CHECK(after.injected);clone_fault(SIZE_MAX);xjson_free(copy);xjson_free(input);size_t leaked=clone_stats().live;
    size_t drained=clone_forced_cleanup();CHECK(drained==leaked);zero();
    if(partial||leaked){printf("BUG clone-oom partial=%u leaked=%zu baseline_live=%zu raw_cleanup_only=true\n",partial?1u:0u,leaked,before.live);return 1;}return 0;
}
static int probe_append(void) {
    clone_fault(SIZE_MAX);XrJsonValue *object=xjson_new_object(),*value=xjson_new_number(7);CHECK(object&&value);
    clone_fault(1);xjson_object_set_new(object,"key",value);CHECK(clone_stats().injected);
    int count=object->as.object.count;bool null_key=count&&object->as.object.members[0].key==NULL;
    clone_fault(SIZE_MAX);if(!count)xjson_free(value);xjson_free(object);zero();
    if(count||null_key){printf("BUG append-oom count=%d null_key=%u\n",count,null_key?1u:0u);return 1;}return 0;
}
int main(int argc,char **argv) {
    if(argc==2&&!strcmp(argv[1],"--probe-key"))return probe_key();
    if(argc==2&&!strcmp(argv[1],"--probe-clone"))return probe_clone();
    if(argc==2&&!strcmp(argv[1],"--probe-append"))return probe_append();
    CHECK(argc==1||(argc==2&&!strcmp(argv[1],"--normal")));bool full=argc==1;size_t checked=0;
    for(unsigned mode=0;mode<sizeof(cases)/sizeof(*cases);++mode)checked+=clone_case(cases[mode],mode,full);
    char deep[XJSON_MAX_DEPTH*2+1];size_t depth=XJSON_MAX_DEPTH-1;
    memset(deep,'[',depth);deep[depth]='0';memset(deep+depth+1,']',depth);deep[depth*2+1]=0;
    checked+=clone_case(deep,7,full);append_cases();clone_boundaries();zero();
    printf("Clone cases8 FI_checked=%zu full=%u parser_depth=%u physical0/0 PASS\n",checked,full?1u:0u,XJSON_MAX_DEPTH);return 0;
}
