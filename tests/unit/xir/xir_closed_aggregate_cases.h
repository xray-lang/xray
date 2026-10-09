/*
 * xray - Lightweight typed scripting with native concurrency
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_closed_aggregate_cases.h - Complete escaped graphs and callback publication
 */
#ifndef XIR_CLOSED_AGGREGATE_CASES_H
#define XIR_CLOSED_AGGREGATE_CASES_H
#include "xir/xxir_tuple.h"
#include "xir/xxir_nullable.h"

static void closed_optional_shape(const XrXirValue *outer, unsigned state) {
    bool some = false; const XrXirValue *inner = NULL, *text = NULL;
    CHECK(xr_xir_nullable_view(outer,&some,&inner) && some == (state != 0));
    if (!state) return;
    CHECK(inner && xr_xir_nullable_view(inner,&some,&text) && some == (state == 2));
    if (state == 2) { CHECK(text); closed_string(text); }
}
static void closed_tuple_nullable(void) {
    XrXirCallableParameter fields[] = {{XR_XIR_STRING,0},{XR_XIR_STRING,0},{XR_XIR_UNIT,0},
        {XR_XIR_I64,0},{(XrXirType)257,0},{(XrXirType)257,0},{(XrXirType)257,0}};
    XrXirTypeNode nodes[] = {
        {.kind=XR_XIR_TYPE_NULLABLE,.element=XR_XIR_STRING},
        {.kind=XR_XIR_TYPE_NULLABLE,.element=(XrXirType)256},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=fields,.parameter_count=7}};
    XrXirTypes types = {nodes,3,NULL,NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types,100,value_compile_limits(65536,65536,10000),&arena) == XR_XIR_VALUE_OK);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena,domain,NULL,NULL,100000,65536};
    XrXirValue text = {0}, inner[2] = {{0}}, outer[3] = {{0}}, tuple = {0}, alias = {0};
    CHECK(xr_xir_string_new(domain,"kept",4,&text) == XR_XIR_VALUE_OK);
    XirObject *text_object = object_pointer(&text);
    CHECK(xr_xir_nullable_new((XrXirType)256,NULL,&admission,&inner[0]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)256,&text,&admission,&inner[1]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257,NULL,&admission,&outer[0]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257,&inner[0],&admission,&outer[1]) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_nullable_new((XrXirType)257,&inner[1],&admission,&outer[2]) == XR_XIR_VALUE_OK);
    XrXirValue values[] = {text,text,{0},{XR_XIR_I64,0,73},outer[0],outer[1],outer[2]};
    CHECK(xr_xir_tuple_new((XrXirType)258,values,7,&admission,&tuple) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&tuple,&alias) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&text);
    for (unsigned i=0;i<2;++i) xr_xir_value_drop(&inner[i]);
    for (unsigned i=0;i<3;++i) xr_xir_value_drop(&outer[i]);
    CHECK(atomic_load(&text_object->references) == 3); /* Two tuple slots and Some(String). */
    xr_xir_compile_type_arena_drop(arena); arena = NULL;
    size_t before; closed_no_alloc_begin(&before); xr_xir_domain_close(domain);
    CHECK(atomic_load(&text_object->references) == 3);
    XrXirValue escaped = {0};
    CHECK(xr_xir_tuple_get(&alias,0,&escaped) == XR_XIR_VALUE_OK); closed_string(&escaped);
    for (uint32_t i=1;i<7;++i) {
        XrXirValue item = {0}; CHECK(xr_xir_tuple_get(&alias,i,&item) == XR_XIR_VALUE_OK);
        if (i==1) closed_string(&item);
        else if (i==2) CHECK(!item.type && !item.reserved && !item.payload);
        else if (i==3) CHECK(item.type == XR_XIR_I64 && item.payload == 73);
        else closed_optional_shape(&item,i-4);
        xr_xir_value_drop(&item);
    }
    xr_xir_value_drop(&tuple); xr_xir_domain_drop(domain);
    xr_xir_value_drop(&alias); closed_string(&escaped);
    xr_xir_value_drop(&escaped); CHECK(!live); closed_no_alloc_end(before);
}

static void closed_class_array(void) {
    for (unsigned order=0;order<2;++order) {
        XrXirDomain *domains[2] = {NULL,NULL};
        for (unsigned i=0;i<2;++i) CHECK(xr_xir_domain_new(1048576,&domains[i]) == XR_XIR_VALUE_OK);
        XrXirTypeArena *arena = class_array_field_arena(domains[0]);
        XrXirValueAdmission local = {arena,domains[0],NULL,NULL,1000000,65536};
        XrXirValueAdmission remote = {arena,domains[1],NULL,NULL,1000000,65536};
        XrXirValue text = {0}, texts = {0}, numbers = {0}, replacement = {0}, object = {0}, alias = {0};
        XrXirValue forty = {XR_XIR_I64,0,40};
        CHECK(xr_xir_string_new(domains[1],"kept",4,&text) == XR_XIR_VALUE_OK);
        XrXirValue repeated[] = {text,text};
        CHECK(xr_xir_array_new((XrXirType)257,repeated,2,&remote,&texts) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_array_new((XrXirType)257,&text,1,&remote,&replacement) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_array_new((XrXirType)256,&forty,1,&remote,&numbers) == XR_XIR_VALUE_OK);
        XrXirValue fields[] = {{XR_XIR_BOOL,0,1},numbers,texts};
        CHECK(xr_xir_class_new((XrXirType)258,fields,3,&local,&object) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_value_copy(&object,&alias) == XR_XIR_VALUE_OK && alias.payload == object.payload);
        xr_xir_value_drop(&text); xr_xir_value_drop(&numbers); xr_xir_value_drop(&texts);
        size_t before; closed_no_alloc_begin(&before);
        xr_xir_domain_close(domains[order]); xr_xir_domain_close(domains[1u-order]);
        closed_no_alloc_end(before);
        XrXirValue old = {0}, observed = {0}, item = {0}; XrXirFaultDetail fault = {0}; int64_t count = -1;
        CHECK(xr_xir_class_get(&alias,2,&local,&old) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_array_len(&old,&local,&count) == XR_XIR_VALUE_OK && count == 2);
        for (int64_t i=0;i<2;++i) {
            CHECK(xr_xir_array_get(&old,i,&local,&item,&fault) == XR_XIR_VALUE_OK);
            closed_string(&item); xr_xir_value_drop(&item);
        }
        CHECK(xr_xir_class_set(&object,2,&replacement,&local) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_class_get(&alias,2,&local,&observed) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_array_len(&observed,&local,&count) == XR_XIR_VALUE_OK && count == 1);
        CHECK(xr_xir_array_get(&observed,0,&local,&item,&fault) == XR_XIR_VALUE_OK); closed_string(&item);
        CHECK(xr_xir_array_len(&old,&local,&count) == XR_XIR_VALUE_OK && count == 2);
        xr_xir_value_drop(&old); xr_xir_value_drop(&observed); xr_xir_value_drop(&replacement);
        closed_no_alloc_begin(&before);
        xr_xir_compile_type_arena_drop(arena);
        xr_xir_domain_drop(domains[0]); xr_xir_domain_drop(domains[1]);
        xr_xir_value_drop(&object); xr_xir_value_drop(&alias); closed_string(&item);
        xr_xir_value_drop(&item); CHECK(!live); closed_no_alloc_end(before);
    }
}

typedef struct ClosedReentry { ClosedRing *peer; XrXirValue *text; size_t releases; bool armed; } ClosedReentry;
static void closed_reentry_release(void *owner) {
    ClosedReentry *r = owner; ++r->releases;
    if (!r->armed) return;
    CHECK(r->releases == 1);
    XrXirValue copy = {0}; CHECK(xr_xir_value_copy(r->text,&copy) == XR_XIR_VALUE_OK);
    closed_string(&copy); xr_xir_domain_close(r->peer->domain); xr_xir_value_drop(&copy);
}
static XrXirValueStatus closed_reentry_admit(void *context,const XrXirFunctionBinding *binding,
    XrXirType type,uint64_t *work) {
    uint64_t cost = (uint64_t)binding->capture_count + 1;
    if (*work < cost) return XR_XIR_VALUE_LIMIT;
    *work -= cost;
    return binding->owner == context && binding->release == closed_reentry_release &&
        binding->entry == 0 && type == (XrXirType)256 ? XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
static void closed_callback_reentry(void) {
    ClosedRing peer; closed_ring_new(&peer,1,false);
    XrXirValue text = {0}; CHECK(xr_xir_string_new(peer.domain,"kept",4,&text) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&peer.cell); xr_xir_value_drop(&peer.function);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = allocation_arena(domain);
    ClosedReentry witness = {&peer,&text,0,false};
    XrXirValueAdmission admission = {arena,domain,closed_reentry_admit,&witness,100000,65536};
    XrXirValue seed = {0}, cell = {0}, function = {0};
    XrXirFunctionBinding binding = {&witness,closed_reentry_release,0,NULL,0};
    CHECK(xr_xir_function_new(domain,arena,(XrXirType)256,&binding,&admission,&seed) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain,arena,(XrXirType)258,&seed,&admission,&cell) == XR_XIR_VALUE_OK);
    binding.captures = &cell; binding.capture_count = 1;
    CHECK(xr_xir_function_new(domain,arena,(XrXirType)256,&binding,&admission,&function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_write(&cell,&function,&admission) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&seed); CHECK(witness.releases == 1); witness.releases = 0; witness.armed = true;
    xr_xir_value_drop(&cell); xr_xir_value_drop(&function);
    size_t before; closed_no_alloc_begin(&before); xr_xir_domain_close(domain);
    CHECK(witness.releases == 1 && peer.releases == 1); closed_string(&text);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain);
    closed_ring_owners_drop(&peer); xr_xir_value_drop(&text);
    CHECK(!live); closed_no_alloc_end(before);
}

typedef struct ClassReentry ClassReentry;
typedef struct ClassReleaseOwner { ClassReentry *test; bool old; } ClassReleaseOwner;
struct ClassReentry {
    ClassReleaseOwner owner[2]; XrXirValue *object; XrXirValueAdmission *admission;
    uint32_t released[2], observations; bool armed;
};
static void class_reentry_release(void *owner) {
    ClassReleaseOwner *slot = owner; ClassReentry *test = slot->test;
    ++test->released[slot->old ? 0 : 1];
    if (!slot->old || !test->armed) return;
    XrXirValue array = {0}, tuple = {0}, function = {0}, number = {0}; XrXirFaultDetail fault = {0};
    CHECK(xr_xir_class_get(test->object,0,test->admission,&array) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_array_get(&array,0,test->admission,&tuple,&fault) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(&tuple,0,&function) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_tuple_get(&tuple,1,&number) == XR_XIR_VALUE_OK);
    const XrXirFunctionBinding *binding = xr_xir_function_binding(&function);
    CHECK(binding && binding->entry == 1 && binding->owner == &test->owner[1]);
    CHECK(number.type == XR_XIR_I64 && number.payload == 73); ++test->observations;
    xr_xir_value_drop(&number); xr_xir_value_drop(&function);
    xr_xir_value_drop(&tuple); xr_xir_value_drop(&array);
}
static XrXirValueStatus class_reentry_admit(void *context,const XrXirFunctionBinding *binding,
    XrXirType type,uint64_t *work) {
    if (!*work) return XR_XIR_VALUE_LIMIT;
    --*work; ClassReentry *test = context;
    return type == (XrXirType)256 && binding->entry < 2 && !binding->capture_count &&
        binding->owner == &test->owner[binding->entry] && binding->release == class_reentry_release ?
        XR_XIR_VALUE_OK : XR_XIR_VALUE_BAD_ARGUMENT;
}
static void closed_class_publish_before_callback(void) {
    XrXirNominalFieldIdentity field = {{"items",5},XR_XIR_FIELD_MUTABLE};
    XrXirNominalIdentity identity = {{"module",6},{"CallbackBox",11},1,0,&field,1,XR_XIR_NOMINAL_CLASS,NULL,0,XR_XIR_NOMINAL_FINAL,{0}};
    XrXirNominalTable table = {NULL,1,&identity}; XrXirType member = (XrXirType)258;
    XrXirCallableParameter tuple_fields[] = {{(XrXirType)256,0},{XR_XIR_I64,0}};
    XrXirTypeNode nodes[] = {
        {.kind=XR_XIR_TYPE_CALLABLE,.result=XR_XIR_I64,.flags=XR_XIR_CALLABLE_ROOT_UNRESOLVED},
        {.kind=XR_XIR_TYPE_TUPLE,.parameters=tuple_fields,.parameter_count=2},
        {.kind=XR_XIR_TYPE_ARRAY,.element=(XrXirType)257},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,&member,1}}};
    XrXirTypes types = {nodes,4,&table,NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types,100,value_compile_limits(65536,65536,10000),&arena) == XR_XIR_VALUE_OK);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(65536,&domain) == XR_XIR_VALUE_OK);
    ClassReentry test = {0}; XrXirValue object = {0}, arrays[2] = {{0}};
    XrXirValueAdmission admission = {arena,domain,class_reentry_admit,&test,1000000,65536};
    test.object = &object; test.admission = &admission;
    for (unsigned i=0;i<2;++i) {
        test.owner[i] = (ClassReleaseOwner){&test,i==0};
        XrXirFunctionBinding binding = {&test.owner[i],class_reentry_release,i,NULL,0};
        XrXirValue function = {0}, tuple = {0};
        CHECK(xr_xir_function_new(domain,arena,(XrXirType)256,&binding,&admission,&function) == XR_XIR_VALUE_OK);
        XrXirValue values[] = {function,{XR_XIR_I64,0,i ? 73 : 11}};
        CHECK(xr_xir_tuple_new((XrXirType)257,values,2,&admission,&tuple) == XR_XIR_VALUE_OK);
        CHECK(xr_xir_array_new((XrXirType)258,&tuple,1,&admission,&arrays[i]) == XR_XIR_VALUE_OK);
        xr_xir_value_drop(&tuple); xr_xir_value_drop(&function);
    }
    CHECK(xr_xir_class_new((XrXirType)259,&arrays[0],1,&admission,&object) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&arrays[0]); CHECK(!test.released[0] && !test.released[1]);
    xr_xir_domain_close(domain); test.armed = true;
    CHECK(xr_xir_class_set(&object,0,&arrays[1],&admission) == XR_XIR_VALUE_OK);
    CHECK(test.released[0] == 1 && !test.released[1] && test.observations == 1);
    test.armed = false; xr_xir_value_drop(&arrays[1]);
    xr_xir_compile_type_arena_drop(arena); xr_xir_domain_drop(domain); xr_xir_value_drop(&object);
    CHECK(test.released[0] == 1 && test.released[1] == 1 && !live);
}
static void closed_aggregate_cases(void) {
    CHECK(!live && !closed_fail_all); fail_at = SIZE_MAX;
    closed_tuple_nullable(); closed_class_array(); closed_callback_reentry(); closed_class_publish_before_callback();
    CHECK(!live && !value_compile_live && !value_compile_bytes);
    puts("Closed aggregate graphs: duplicate tuple edges, nullable tags, foreign arrays, mutable class publication and callback reentry physical0");
}
#endif // XIR_CLOSED_AGGREGATE_CASES_H
