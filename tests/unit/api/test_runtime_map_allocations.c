/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_runtime_map_allocations.c - Runtime map creation and growth failures
 *
 * KEY CONCEPT:
 *   Real constructors and map mutations preserve their owned publication and
 *   borrowed values under each fixed allocator failure and final teardown.
 */
#include "xr_runtime_map_probe.h"
#include "runtime/xisolate_api.h"
#include "runtime/core/xr_exec_context.h"
#include "runtime/core/xr_runtime_core.h"
#include "runtime/class/xclass_system.h"
#include "runtime/object/xstring.h"
#include "runtime/symbol/xsymbol_table.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "mode=%d fail=%zu attempts=%zu line=%d: %s\n", \
        (int)mode, fail_at, attempts, __LINE__, #c); exit(1); \
} } while (0)

typedef enum Tag { MAP_NONE, MAP_GLOBAL, MAP_STRINGS, MAP_GROW } Tag;
typedef union MapOwnerModel {
    XrOsIoPolicy policy;
    long double alignment;
    void *pointer;
    uint64_t integer;
} MapOwnerModel;
typedef struct Expected { size_t bytes; const char *source; } Expected;
typedef struct Allocation { void *memory; size_t bytes, serial; } Allocation;
static const Expected global_plan[] = {
    {sizeof(XrGlobalObject), "xglobal_object.c"},
    {sizeof(MapOwnerModel)+sizeof(XrHashMap), "xio_policy.c"},
    {16*sizeof(XrHashMapEntry), "xio_policy.c"},
    {sizeof(MapOwnerModel)+sizeof(XrHashMap), "xio_policy.c"},
    {16*sizeof(XrHashMapEntry), "xio_policy.c"}
};
static const Expected string_plan[] = {
    {sizeof(MapOwnerModel)+sizeof(XrHashMap), "xio_policy.c"},
    {16*sizeof(XrHashMapEntry), "xio_policy.c"}
};
static const Expected growth_plan[] = {{32*sizeof(XrHashMapEntry), "xio_policy.c"}};
static Tag mode;
static XR_THREAD_LOCAL Tag active;
static atomic_flag observer_lock = ATOMIC_FLAG_INIT;
static Allocation live[16];
static size_t blocks, bytes_live, serial, releases[16], release_count;
static size_t attempts, fail_at, rejects, engine_calls, global_calls;
static size_t expected_count;
static bool capture_registration;
static const char *const keys[] = {
    "Owner00", "Owner01", "Owner02", "Owner03", "Owner04", "Owner05", "Owner06",
    "Owner07", "Owner08", "Owner09", "Owner10", "Owner11", "Owner12"
};

static void lock_observer(void) {
    while (atomic_flag_test_and_set_explicit(&observer_lock,memory_order_acquire)) { }
}
static void unlock_observer(void) {
    atomic_flag_clear_explicit(&observer_lock,memory_order_release);
}
static size_t slot(void *memory) {
    for (size_t i=0;i<16;++i) if (live[i].memory==memory) return i;
    return 16;
}
static const char *basename_of(const char *path) {
    const char *name=path;
    for (;*path;++path) if (*path=='/' || *path=='\\') name=path+1;
    return name;
}
XR_FUNC void *xr_runtime_map_malloc(size_t bytes,const char *source) {
    if (active==MAP_NONE) return xr_malloc(bytes);
    lock_observer();
    CHECK(active==mode && attempts<expected_count);
    const Expected *plan=active==MAP_GLOBAL?global_plan:active==MAP_STRINGS?string_plan:growth_plan;
    Expected expected=plan[attempts++];
    CHECK(bytes==expected.bytes && !strcmp(expected.source,basename_of(source)));
    bool rejected=attempts==fail_at;
    if (rejected) ++rejects;
    unlock_observer();
    if (rejected) return NULL;
    void *memory=xr_malloc(bytes);CHECK(memory);
    lock_observer();
    size_t index=slot(NULL);CHECK(index<16 && bytes_live<=SIZE_MAX-bytes);
    live[index]=(Allocation){memory,bytes,++serial};++blocks;bytes_live+=bytes;
    unlock_observer();return memory;
}
XR_FUNC void *xr_runtime_map_calloc(size_t count,size_t bytes) {
    CHECK(active==MAP_NONE);return xr_calloc(count,bytes);
}
XR_FUNC void *xr_runtime_map_realloc(void *memory,size_t bytes) {
    CHECK(active==MAP_NONE);
    lock_observer();bool tracked=memory && slot(memory)<16;unlock_observer();
    CHECK(!tracked);return xr_realloc(memory,bytes);
}
XR_FUNC void xr_runtime_map_free(void *memory) {
    if (!memory) return;
    lock_observer();size_t index=slot(memory);
    CHECK(active==MAP_NONE || index<16);
    if (index<16) {
        CHECK(blocks && bytes_live>=live[index].bytes && release_count<16);
        releases[release_count++]=live[index].serial;
        --blocks;bytes_live-=live[index].bytes;live[index]=(Allocation){0};
    }
    unlock_observer();xr_free(memory);
}
XR_FUNC XrOsIoStatus xr_runtime_map_strings_new(const XrOsIoPolicy *policy,XrHashMap **output) {
    CHECK(active==MAP_NONE);
    if (mode==MAP_STRINGS) active=MAP_STRINGS;
    XrOsIoStatus status=xr_hashmap_owned_new(policy,output);
    active=MAP_NONE;return status;
}
XR_FUNC XrOsIoStatus xr_runtime_map_registered_set(XrHashMap *map,const char *key,void *value) {
    CHECK(active==MAP_NONE);
    if (capture_registration) active=MAP_GROW;
    XrOsIoStatus status=xr_hashmap_owned_set(map,key,value);
    active=MAP_NONE;return status;
}
XR_FUNC int xr_runtime_map_engine_init(XrVMRuntime *runtime) {
    ++engine_calls;CHECK(runtime && !runtime->vm.strings_map);
    int result=xr_execution_engine_init(runtime);
    if (result) CHECK(!runtime->vm.strings_map);
    else CHECK(runtime->vm.strings_map);
    return result;
}
XR_FUNC XrGlobalObject *xr_runtime_map_global_create(XrVMRuntime *runtime) {
    ++global_calls;CHECK(runtime && active==MAP_NONE);
    void *prior=runtime->global_object;
    if (mode==MAP_GLOBAL) active=MAP_GLOBAL;
    XrGlobalObject *result=xr_global_object_create(runtime);
    active=MAP_NONE;
    CHECK(runtime->global_object==prior);
    if (result) CHECK(result->isolate==runtime && result->properties && result->functions);
    return result;
}
static void phase(Tag next,size_t expected,size_t failure) {
    CHECK(active==MAP_NONE);mode=next;expected_count=expected;fail_at=failure;
    attempts=rejects=release_count=0;capture_registration=false;
}
static void empty(void) {
    CHECK(!blocks && !bytes_live && active==MAP_NONE);
    for (size_t i=0;i<16;++i) CHECK(!live[i].memory);
}
static void reset(void) {
    empty();serial=engine_calls=global_calls=0;phase(MAP_NONE,0,0);
}
static void observed(const char *label) {
    printf("%s mode=%d fail=%zu attempts=%zu rejects=%zu blocks=%zu bytes=%zu releases=%zu\n",
        label,(int)mode,fail_at,attempts,rejects,blocks,bytes_live,release_count);
}
static XrVMRuntime *new_vm(void) {
    XrVMConfig config={0};return xray_vm_new_full(&config);
}
static void deleted(void) {
    empty();CHECK(!g_current_isolate && !xr_exec_context_current());
}
static void constructor_failures(Tag selected,size_t count) {
    static const size_t global_order[][4]={{0},{1},{2,1},{3,2,1},{4,3,2,1}};
    for (size_t failure=1;failure<=count;++failure) {
        reset();CHECK(!g_current_isolate && !xr_exec_context_current());
        phase(selected,count,failure);XrVMRuntime *runtime=new_vm();
        CHECK(!runtime && rejects==1 && attempts==failure && engine_calls==1);
        CHECK(global_calls==(selected==MAP_GLOBAL?1u:0u));
        CHECK(release_count==failure-1);
        for (size_t i=0;i<release_count;++i)
            CHECK(releases[i]==(selected==MAP_GLOBAL?global_order[failure-1][i]:1u));
        deleted();observed("constructor-refused");
        /* Retry a fresh real constructor in the same process, with no failure. */
        reset();runtime=new_vm();CHECK(runtime);xray_vm_delete(runtime);deleted();
    }
}
static void entries(XrHashMap *map,void *const *values,size_t count) {
    CHECK(xr_hashmap_count(map)==count);
    for (size_t i=0;i<count;++i) {
        void *value=NULL;CHECK(xr_hashmap_owned_get(map,keys[i],&value)==XR_OS_IO_OK);
        CHECK(value==values[i]);
    }
}
static void builtin_ids(XrVMRuntime *runtime) {
    XrSymbolTable *table=xr_isolate_get_symbol_table(runtime);CHECK(table);
    CHECK(xr_symbol_lookup_in_table(table,"length")==SYMBOL_LENGTH);
    CHECK(xr_symbol_lookup_in_table(table,"get")==SYMBOL_GET);
    CHECK(!strcmp(xr_symbol_get_name_in_table(table,SYMBOL_LENGTH),"length"));
    CHECK(!strcmp(xr_symbol_get_name_in_table(table,SYMBOL_GET),"get"));
}
static void string_values(void *const *values) {
    for (size_t i=0;i<13;++i) {
        XrString *value=values[i];
        CHECK(value->length==strlen(keys[i]));
        CHECK(!memcmp(value->data,keys[i],strlen(keys[i])+1));
    }
}
static void global_growth(void) {
    reset();XrVMRuntime *runtime=new_vm();CHECK(runtime);
    builtin_ids(runtime);
    XrGlobalObject *published=runtime->global_object;
    phase(MAP_GLOBAL,5,0);XrGlobalObject *global=xr_runtime_map_global_create(runtime);
    CHECK(global && attempts==5 && blocks==5 && !global->registered_class_count);
    CHECK(runtime->global_object==published && global!=published);
    XrClass *klass=xr_isolate_get_core_classes(runtime)->objectClass;CHECK(klass);
    void *values[13];for (size_t i=0;i<13;++i) values[i]=klass;
    for (size_t i=0;i<12;++i) {
        phase(MAP_GROW,0,0);capture_registration=true;
        CHECK(xr_global_register_class(global,keys[i],klass) && !attempts);
        CHECK(global->registered_class_count==(int)i+1);
    }
    entries(global->properties,values,12);
    builtin_ids(runtime);
    XrHashMapEntry *before=global->properties->entries;
    size_t prior_bytes=bytes_live;
    phase(MAP_GROW,1,1);capture_registration=true;
    CHECK(!xr_global_register_class(global,keys[12],klass));
    CHECK(attempts==1 && rejects==1 && !release_count && blocks==5 && bytes_live==prior_bytes);
    CHECK(global->registered_class_count==12 && global->properties->entries==before);
    CHECK(global->properties->capacity==16 && !xr_hashmap_count(global->functions));
    entries(global->properties,values,12);
    void *missing=(void *)klass;
    CHECK(xr_hashmap_owned_get(global->properties,keys[12],&missing)==XR_OS_IO_OK && !missing);
    builtin_ids(runtime);observed("global-growth-refused");
    phase(MAP_GROW,1,0);capture_registration=true;
    CHECK(xr_global_register_class(global,keys[12],klass) && attempts==1 && !rejects);
    CHECK(global->registered_class_count==13 && global->properties->capacity==32);
    CHECK(blocks==5 && bytes_live==prior_bytes+16*sizeof(XrHashMapEntry));
    CHECK(release_count==1 && releases[0]==3);entries(global->properties,values,13);
    builtin_ids(runtime);observed("global-growth-retry");
    phase(MAP_NONE,0,0);xr_global_object_destroy(global);
    static const size_t order[]={6,2,5,4,1};
    CHECK(release_count==5);for(size_t i=0;i<5;++i)CHECK(releases[i]==order[i]);
    empty();CHECK(runtime->global_object==published);observed("global-released");
    xray_vm_delete(runtime);deleted();
}
static void string_growth(void) {
    reset();phase(MAP_STRINGS,2,0);XrVMRuntime *runtime=new_vm();
    CHECK(runtime && attempts==2 && blocks==2 && !rejects);
    XrHashMap *map=runtime->vm.strings_map;
    CHECK(map && map->capacity==16 && !xr_hashmap_count(map));
    builtin_ids(runtime);
    XrExecutionContext *previous=xr_exec_context_enter(xr_runtime_core_root_exec(runtime->core_rt));
    void *values[13];
    for (size_t i=0;i<13;++i) {
        values[i]=xr_string_new(runtime,keys[i],strlen(keys[i]));CHECK(values[i]);
    }
    xr_exec_context_restore(previous);string_values(values);
    for (size_t i=0;i<12;++i) {
        phase(MAP_GROW,0,0);capture_registration=true;
        CHECK(xr_runtime_map_registered_set(map,keys[i],values[i])==XR_OS_IO_OK);
        CHECK(!attempts);
    }
    entries(map,values,12);XrHashMapEntry *before=map->entries;
    size_t prior_bytes=bytes_live;
    phase(MAP_GROW,1,1);capture_registration=true;
    CHECK(xr_runtime_map_registered_set(map,keys[12],values[12])==XR_OS_IO_OUT_OF_MEMORY);
    CHECK(attempts==1 && rejects==1 && !release_count && blocks==2 && bytes_live==prior_bytes);
    CHECK(runtime->vm.strings_map==map && map->entries==before && map->capacity==16);
    entries(map,values,12);void *missing=values[12];
    CHECK(xr_hashmap_owned_get(map,keys[12],&missing)==XR_OS_IO_OK && !missing);
    builtin_ids(runtime);string_values(values);observed("strings-growth-refused");
    phase(MAP_GROW,1,0);capture_registration=true;
    CHECK(xr_runtime_map_registered_set(map,keys[12],values[12])==XR_OS_IO_OK);
    CHECK(attempts==1 && !rejects && blocks==2 && bytes_live==prior_bytes+16*sizeof(XrHashMapEntry));
    CHECK(release_count==1 && releases[0]==2);entries(map,values,13);
    builtin_ids(runtime);string_values(values);observed("strings-growth-retry");
    phase(MAP_NONE,0,0);xray_vm_delete(runtime);deleted();
    CHECK(release_count==2 && releases[0]==3 && releases[1]==1);
    observed("strings-released");
}
int main(void) {
    constructor_failures(MAP_STRINGS,2);
    constructor_failures(MAP_GLOBAL,5);
    global_growth();string_growth();
    puts("runtime maps: seven real constructor refusals, two growth refusals, retry and target physical zero");
    return 0;
}
