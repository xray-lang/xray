/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef JSON_CLONE_BOUNDARIES_H
#define JSON_CLONE_BOUNDARIES_H
#include <limits.h>
static void clone_boundaries(void) {
    clone_fault(SIZE_MAX);CHECK(!xjson_clone(NULL));zero();
    XrJsonValue scalar={.type=XR_JSON_NULL},array={.type=XR_JSON_ARRAY},object={.type=XR_JSON_OBJECT},string={.type=XR_JSON_STRING};
    array.as.array.count=-1;CHECK(!xjson_clone(&array));zero();
    array.as.array.count=1;array.as.array.capacity=0;CHECK(!xjson_clone(&array));zero();
    array.as.array.capacity=1;CHECK(!xjson_clone(&array));zero();
    object.as.object.count=1;CHECK(!xjson_clone(&object));zero();
    object.as.object.capacity=1;CHECK(!xjson_clone(&object));zero();
    string.as.string="x";string.string_len=SIZE_MAX;CHECK(!xjson_clone(&string));zero();
    string.as.string=NULL;string.string_len=1;CHECK(!xjson_clone(&string));zero();
    XrJsonMember member={(char *)"x",SIZE_MAX,&scalar};object.as.object.members=&member;
    CHECK(!xjson_clone(&object));zero();
    XrJsonValue *self=&array;array.as.array.items=&self;array.as.array.count=array.as.array.capacity=1;
    CHECK(!xjson_clone(&array));zero();
    CHECK(!xjson_new_string_n(NULL,1)&&!xjson_new_string_n("x",SIZE_MAX));zero();
    XrJsonValue *empty=xjson_new_string_n(NULL,0);CHECK(empty&&empty->string_len==0);xjson_free(empty);zero();
    array.as.array.count=array.as.array.capacity=INT_MAX;array.as.array.items=&self;
    clone_fault(SIZE_MAX);xjson_array_push(&array,&scalar);CHECK(!clone_stats().attempts&&array.as.array.count==INT_MAX);zero();
    object.as.object.count=object.as.object.capacity=INT_MAX;object.as.object.members=&member;
    xjson_object_set_new(&object,"x",&scalar);CHECK(!clone_stats().attempts&&object.as.object.count==INT_MAX);zero();
    puts("Malformed ownership graphs, cycle/depth, string overflow and container growth boundaries PASS");
}
#endif
