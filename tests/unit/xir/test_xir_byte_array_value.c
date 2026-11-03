/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_byte_array_value.c - Owned bulk byte candidates and physical rollback
 *
 * KEY CONCEPT:
 *   Real owners survive candidate failures; matching shapes grant no identity.
 */
#include "base/xmalloc.h"
#include "xir/xxir_array.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"byte array %d: %s\n",__LINE__,#c); exit(1); } } while (0)

typedef struct ByteAllocation { void *pointer; size_t bytes; } ByteAllocation;
static ByteAllocation byte_blocks[4096];
static size_t byte_attempts, byte_live, byte_live_bytes, byte_fail_at=SIZE_MAX;
static bool byte_injected;
static void byte_record(void *pointer,size_t bytes) {
    if (!pointer) return;
    CHECK(byte_live<sizeof(byte_blocks)/sizeof(byte_blocks[0]) && bytes<=SIZE_MAX-byte_live_bytes);
    byte_blocks[byte_live++]=(ByteAllocation){pointer,bytes};byte_live_bytes+=bytes;
}
static size_t byte_record_index(void *pointer) {
    size_t index=0;
    while (index<byte_live && byte_blocks[index].pointer!=pointer) ++index;
    CHECK(index<byte_live);return index;
}
static bool byte_allocation_fails(void) {
    if (byte_attempts++!=byte_fail_at) return false;
    byte_injected=true;return true;
}
static void *byte_malloc(size_t bytes) {
    if (byte_allocation_fails()) return NULL;
    void *pointer=xr_malloc(bytes);byte_record(pointer,bytes);return pointer;
}
static void *byte_calloc(size_t count,size_t size) {
    CHECK(!size || count<=SIZE_MAX/size);
    if (byte_allocation_fails()) return NULL;
    void *pointer=xr_calloc(count,size);byte_record(pointer,count*size);return pointer;
}
static void byte_free(void *pointer) {
    if (pointer) {
        size_t index=byte_record_index(pointer);
        CHECK(byte_blocks[index].bytes<=byte_live_bytes);
        byte_live_bytes-=byte_blocks[index].bytes;byte_blocks[index]=byte_blocks[--byte_live];
    }
    xr_free(pointer);
}
static void *byte_realloc(void *pointer,size_t bytes) {
    if (byte_allocation_fails()) return NULL;
    size_t index=pointer?byte_record_index(pointer):0;
    void *replacement=xr_realloc(pointer,bytes);
    if (!replacement) return NULL;
    if (!pointer) byte_record(replacement,bytes);
    else {
        CHECK(byte_blocks[index].bytes<=byte_live_bytes);
        byte_live_bytes-=byte_blocks[index].bytes;
        CHECK(bytes<=SIZE_MAX-byte_live_bytes);byte_live_bytes+=bytes;
        byte_blocks[index]=(ByteAllocation){replacement,bytes};
    }
    return replacement;
}
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_realloc
#define xr_malloc byte_malloc
#define xr_calloc byte_calloc
#define xr_free byte_free
#define xr_realloc byte_realloc
#include "base/xcompile_resources.c"
#include "xir/xxir_value.c"
#undef xr_malloc
#undef xr_calloc
#undef xr_free
#undef xr_realloc

typedef struct ByteFixture {
    XrXirCompileContext context;
    XrXirTypeArena *arena;
    XrXirDomain *domain;
    XrXirValueAdmission admission;
    XrXirType bytes;
} ByteFixture;
static ByteFixture byte_fixture_new(bool reordered) {
    ByteFixture f={0};
    const XrCompileResourceLimits limits={XR_COMPILE_MEMORY_ORDINARY,0,true};
    CHECK(xr_compile_resources_new(&limits,&f.context.resources)==XR_COMPILE_RESOURCE_OK);
    f.context.limits=xr_xir_compile_default_limits();
    XrXirTypeNode nodes[]={
        {.kind=XR_XIR_TYPE_ARRAY,.element=reordered?XR_XIR_I64:XR_XIR_U8},
        {.kind=XR_XIR_TYPE_ARRAY,.element=reordered?XR_XIR_U8:XR_XIR_I64}};
    const XrXirTypes types={nodes,2,NULL,NULL};
    CHECK(xr_xir_compile_type_arena_new(&f.context,&types,&f.arena)==XR_XIR_VALUE_OK);
    memset(nodes,0xa5,sizeof(nodes));
    CHECK(xr_xir_domain_new(UINT64_C(1048576),&f.domain)==XR_XIR_VALUE_OK);
    f.admission=(XrXirValueAdmission){f.arena,f.domain,NULL,NULL,UINT64_C(1000000),65536};
    f.bytes=(XrXirType)(XR_XIR_CONSTRUCTED_TYPE_BASE+(reordered?1u:0u));
    return f;
}
static void byte_fixture_free(ByteFixture *f) {
    CHECK(xr_xir_domain_stats(f->domain).live_bytes==sizeof(XrXirDomain));
    xr_xir_domain_drop(f->domain);xr_xir_compile_type_arena_drop(f->arena);
    if (f->context.resources) xr_compile_resources_release(f->context.resources);
    CHECK(!byte_live && !byte_live_bytes);
}
static XrXirValue byte_array_new(ByteFixture *f,const char *bytes,size_t count,int64_t capacity) {
    XrXirValue output={0};
    CHECK(xr_xir_array_with_capacity(f->bytes,capacity,&f->admission,&output)==XR_XIR_VALUE_OK);
    XrXirValuePlace place={f->bytes,&output.payload};
    for (size_t i=0;i<count;++i) {
        XrXirValue item={XR_XIR_U8,0,(unsigned char)bytes[i]};
        CHECK(xr_xir_array_push(&place,&item,&f->admission)==XR_XIR_VALUE_OK);
    }
    return output;
}
static void byte_expect(const XrXirValue *value,const char *bytes,size_t count) {
    CHECK(xr_xir_value_valid(value));
    const XirArray *array=(const XirArray *)object_pointer(value);
    CHECK(array->element==XR_XIR_U8 && array->stride==1 && array->length==count && array->capacity>=count);
    CHECK(!count || !memcmp(array->data,bytes,count));
}
static bool byte_empty(const XrXirValue *value) {
    return !value->type && !value->reserved && !value->payload;
}
static void byte_null_admission(void) {
    ByteFixture f=byte_fixture_new(false);XrXirValue source=byte_array_new(&f,"ab",2,8),out={0};
    const XirArray *array=(const XirArray *)object_pointer(&source);size_t capacity=array->capacity;
    XrXirValue before=source;size_t attempts=byte_attempts,live=byte_live,bytes=byte_live_bytes;
    CHECK(xr_xir_array_byte_append(&source,&source,NULL,&out)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(byte_empty(&out));XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_byte_repeat(&source,1,2,NULL,&out,&fault)==XR_XIR_VALUE_BAD_ARGUMENT);
    CHECK(byte_empty(&out) && xr_xir_fault_empty(fault));
    CHECK(byte_attempts==attempts && byte_live==live && byte_live_bytes==bytes);
    CHECK(!memcmp(&source,&before,sizeof(before)) && array->capacity==capacity);byte_expect(&source,"ab",2);
    xr_xir_value_drop(&source);byte_fixture_free(&f);
}
static void byte_candidates(void) {
    ByteFixture f=byte_fixture_new(false);XrXirValue source=byte_array_new(&f,"ab",2,8),out={0},alias={0};
    const XirArray *array=(const XirArray *)object_pointer(&source);size_t capacity=array->capacity;
    CHECK(xr_xir_array_byte_append(&source,&source,&f.admission,&out)==XR_XIR_VALUE_OK);
    CHECK(out.payload!=source.payload);byte_expect(&out,"abab",4);byte_expect(&source,"ab",2);
    CHECK(((const XirArray *)object_pointer(&out))->capacity==capacity);
    CHECK(xr_xir_value_copy(&out,&alias)==XR_XIR_VALUE_OK);
    XrXirValuePlace place={f.bytes,&out.payload};XrXirValue letter={XR_XIR_U8,0,'Z'};XrXirFaultDetail fault={0};
    CHECK(xr_xir_array_set(&place,0,&letter,&f.admission,&fault)==XR_XIR_VALUE_OK);
    byte_expect(&out,"Zbab",4);byte_expect(&source,"ab",2);byte_expect(&alias,"abab",4);
    xr_xir_value_drop(&out);xr_xir_value_drop(&alias);
    CHECK(xr_xir_value_copy(&source,&alias)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_byte_append(&source,&alias,&f.admission,&out)==XR_XIR_VALUE_OK);
    CHECK(out.payload!=source.payload && out.payload!=alias.payload);byte_expect(&out,"abab",4);
    xr_xir_value_drop(&out);
    CHECK(xr_xir_array_byte_repeat(&source,2,7,&f.admission,&out,&fault)==XR_XIR_VALUE_OK);
    CHECK(xr_xir_fault_empty(fault));byte_expect(&out,"ababababa",9);byte_expect(&source,"ab",2);
    CHECK(((const XirArray *)object_pointer(&out))->capacity>=9 && array->capacity==capacity);
    xr_xir_value_drop(&out);
    CHECK(xr_xir_array_byte_repeat(&source,1,3,&f.admission,&out,&fault)==XR_XIR_VALUE_OK);
    byte_expect(&out,"abbbb",5);xr_xir_value_drop(&out);
    CHECK(xr_xir_array_byte_repeat(&source,2,0,&f.admission,&out,&fault)==XR_XIR_VALUE_OK);
    CHECK(out.payload!=source.payload);byte_expect(&out,"ab",2);xr_xir_value_drop(&out);
    const int64_t distances[]={0,-1,3};
    for (size_t i=0;i<sizeof(distances)/sizeof(distances[0]);++i) {
        CHECK(xr_xir_array_byte_repeat(&source,distances[i],0,&f.admission,&out,&fault)==XR_XIR_VALUE_BOUNDS);
        CHECK(byte_empty(&out) && xr_xir_fault_bounds_valid(fault));byte_expect(&source,"ab",2);
    }
    xr_xir_value_drop(&alias);xr_xir_value_drop(&source);byte_fixture_free(&f);
}
static XrXirValueStatus byte_operate(bool repeat,const XrXirValue *source,
    XrXirValueAdmission *admission,XrXirValue *out) {
    XrXirFaultDetail fault={0};
    XrXirValueStatus status=repeat?xr_xir_array_byte_repeat(source,2,7,admission,out,&fault):
        xr_xir_array_byte_append(source,source,admission,out);
    CHECK(xr_xir_fault_empty(fault));return status;
}
static size_t byte_failure(bool repeat,size_t point,bool limit) {
    ByteFixture f=byte_fixture_new(false);XrXirValue source=byte_array_new(&f,"ab",2,8),out={0};
    XrXirValue original=source;const XirArray *array=(const XirArray *)object_pointer(&source);
    size_t capacity=array->capacity,live=byte_live,bytes=byte_live_bytes,begin=byte_attempts;
    uint64_t held=xr_xir_domain_stats(f.domain).live_bytes;
    if (limit) f.admission.work=0;
    byte_injected=false;
    if (point!=SIZE_MAX) {CHECK(point<=SIZE_MAX-begin);byte_fail_at=begin+point;}
    XrXirValueStatus status=byte_operate(repeat,&source,&f.admission,&out);
    size_t attempts=byte_attempts-begin;byte_fail_at=SIZE_MAX;
    CHECK(!memcmp(&source,&original,sizeof(source)) && array->capacity==capacity);byte_expect(&source,"ab",2);
    if (point==SIZE_MAX && !limit) {
        CHECK(status==XR_XIR_VALUE_OK && attempts && out.payload!=source.payload);
        byte_expect(&out,repeat?"ababababa":"abab",repeat?9u:4u);
    } else {
        CHECK(status==(limit?XR_XIR_VALUE_LIMIT:XR_XIR_VALUE_OOM) && byte_empty(&out));
        CHECK(limit?!byte_injected:byte_injected);
        CHECK(byte_live==live && byte_live_bytes==bytes && xr_xir_domain_stats(f.domain).live_bytes==held);
    }
    xr_xir_value_drop(&out);xr_xir_value_drop(&source);byte_fixture_free(&f);return attempts;
}
static void byte_unrelated_arenas(void) {
    ByteFixture a=byte_fixture_new(false),b=byte_fixture_new(true);
    XrXirValue left=byte_array_new(&a,"ab",2,8),right=byte_array_new(&b,"cd",2,8),out={0};
    CHECK(left.type!=right.type && !xr_xir_value_argument(&right,a.arena,a.bytes));
    CHECK(xr_xir_array_byte_append(&left,&right,&a.admission,&out)==XR_XIR_VALUE_BAD_ARGUMENT && byte_empty(&out));
    CHECK(byte_operate(true,&right,&a.admission,&out)==XR_XIR_VALUE_BAD_ARGUMENT && byte_empty(&out));
    byte_expect(&left,"ab",2);byte_expect(&right,"cd",2);
    xr_xir_value_drop(&left);xr_xir_value_drop(&right);
    xr_xir_domain_drop(a.domain);xr_xir_compile_type_arena_drop(a.arena);xr_compile_resources_release(a.context.resources);
    byte_fixture_free(&b);
}
static void byte_retired_owner(void) {
    ByteFixture f=byte_fixture_new(false);XrXirValue source=byte_array_new(&f,"ab",2,8),out={0};
    const XrXirTypeArena *retained=xr_xir_value_arena(&source);CHECK(retained==f.arena);
    xr_xir_compile_type_arena_drop(f.arena);f.arena=NULL;
    xr_compile_resources_release(f.context.resources);f.context.resources=NULL;
    f.admission.arena=retained;
    CHECK(xr_xir_array_byte_append(&source,&source,&f.admission,&out)==XR_XIR_VALUE_OK);
    xr_xir_value_drop(&source);byte_expect(&out,"abab",4);
    xr_xir_value_drop(&out);byte_fixture_free(&f);
}
int main(void) {
    byte_null_admission();byte_candidates();byte_unrelated_arenas();byte_retired_owner();
    size_t sites[2]={0,0};
    for (unsigned repeat=0;repeat<2;++repeat) {
        sites[repeat]=byte_failure(repeat!=0,SIZE_MAX,false);
        for (size_t point=0;point<sites[repeat];++point) byte_failure(repeat!=0,point,false);
        byte_failure(repeat!=0,SIZE_MAX,true);
    }
    CHECK(!byte_live && !byte_live_bytes);
    printf("Byte candidates: owned/COW/self snapshot, overlapping repeat, bounds, arena refusal, retirement; OOM sites %zu/%zu; physical0/0 PASS\n",sites[0],sites[1]);
    return 0;
}
