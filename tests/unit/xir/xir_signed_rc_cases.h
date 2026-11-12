/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_signed_rc_cases.h - Local, shared and identity-owned value transitions
 */
#ifndef XIR_SIGNED_RC_CASES_H
#define XIR_SIGNED_RC_CASES_H
static void signed_string_bands(void) {
    XrXirDomain *domain = NULL;
    XrXirValue text = {0}, suffix = {0}, alias = {0};
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "band", 4, &text) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_string_new(domain, "!", 1, &suffix) == XR_XIR_VALUE_OK);
    XirObject *object = object_pointer(&text);
    CHECK(xir_object_references(object) == 1);
    CHECK(xr_xir_value_share(&text) == XR_XIR_VALUE_OK && xir_object_references(object) == -1);
    CHECK(xr_xir_string_append(&text, &suffix) == XR_XIR_VALUE_OK);
    CHECK(object_pointer(&text) == object && xir_object_references(object) == 1);
    CHECK(xr_xir_value_share(&text) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_value_copy(&text, &alias) == XR_XIR_VALUE_OK && xir_object_references(object) == -2);
    CHECK(xr_xir_string_append(&text, &suffix) == XR_XIR_VALUE_OK);
    CHECK(object_pointer(&text) != object && xir_object_references(object_pointer(&text)) == 1);
    const char *bytes = NULL; size_t length = 0;
    CHECK(xr_xir_string_view(&alias, &bytes, &length) && length == 5 && !memcmp(bytes, "band!", 5));
    CHECK(xr_xir_string_view(&text, &bytes, &length) && length == 6 && !memcmp(bytes, "band!!", 6));
    for (unsigned shared = 0; shared < 2; ++shared) {
        int32_t saved = xir_object_references(object);
        atomic_store(&object->references, shared ? -INT32_MAX : INT32_MAX);
        XrXirValue rejected = {0};
        CHECK(xr_xir_value_copy(&alias, &rejected) == XR_XIR_VALUE_REFCOUNT_LIMIT && !rejected.type);
        atomic_store(&object->references, saved);
    }
    xr_xir_value_drop(&alias); xr_xir_value_drop(&text); xr_xir_value_drop(&suffix);
    xr_xir_domain_drop(domain);
    CHECK(!live);
}
static size_t signed_shared_cell_place(size_t fail_point) {
    XrXirTypeNode nodes[] = {{.kind=XR_XIR_TYPE_ARRAY, .element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_CELL, .element=(XrXirType)256}};
    XrXirTypes types = {nodes, 2, NULL, NULL};
    XrXirTypeArena *arena = NULL; XrXirDomain *domain = NULL;
    CHECK(value_compile_arena(&types, 65536, value_compile_limits(), &arena) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_domain_new(65536, &domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena, domain, NULL, NULL, 65536, 65536};
    XrXirValue number = {XR_XIR_I64, 0, 17}, array = {0}, cell = {0}, output = {0};
    CHECK(xr_xir_array_new((XrXirType)256, &number, 1, &admission, &array) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_cell_new(domain, arena, (XrXirType)257, &array, &admission, &cell) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&array);
    CHECK(xr_xir_value_share(&cell) == XR_XIR_VALUE_OK);
    XirCell *holder = (XirCell *)object_pointer(&cell);
    XirObject *before = object_pointer(&holder->value);
    CHECK(xir_object_references(before) == -1);
    XrXirValuePlace place = {0};
    CHECK(xr_xir_cell_authorized_place(&cell, NULL, &admission, &place) == XR_XIR_VALUE_OK);
    CHECK(place.identity == &holder->object);
    number.payload = 29;
    CHECK(xr_xir_array_push(&place, &number, &admission) == XR_XIR_VALUE_OK);
    CHECK(object_pointer(&holder->value) != before && xir_object_references(object_pointer(&holder->value)) == -1);
    XrXirFaultDetail fault = {0};
    CHECK(xr_xir_array_get(&holder->value, 1, &admission, &output, &fault) == XR_XIR_VALUE_OK && output.payload == 29);
    xr_xir_value_drop(&output);
    XrXirCellAuthority authority = {&cell, &admission, 1};
    XrXirViewStorageSpec spec = {authority, 0, 2, XR_XIR_TYPE_MUT_SLICE};
    XrXirViewStorage view = {0};
    CHECK(xr_xir_view_storage_begin(&cell, NULL, &spec, &admission, &view) == XR_XIR_VALUE_OK);
    CHECK(xir_object_references(object_pointer(&holder->value)) == -1);
    XrXirCellAuthority child_authority = {&cell, &view, 2};
    XrXirViewStorageSpec child_spec = {child_authority, 0, 1, XR_XIR_TYPE_MUT_SLICE};
    XrXirViewStorage child = {0};
    uint64_t work = admission.work; admission.work = 0;
    CHECK(xr_xir_view_storage_reborrow(&view, &authority, &child_spec, &admission, &child) == XR_XIR_VALUE_LIMIT);
    admission.work = work;
    CHECK(view_storage_empty(&child) && xir_object_references(object_pointer(&holder->value)) == -1);
    CHECK(xr_xir_view_storage_reborrow(&view, &authority, &child_spec, &admission, &child) == XR_XIR_VALUE_OK);
    CHECK(xir_object_references(object_pointer(&holder->value)) == -1);
    xr_xir_view_storage_end(&child);
    number.payload = 43;
    int64_t original = holder->value.payload;
    size_t begin = value_allocation_calls, blocks = live;
    uint64_t bytes = xr_xir_domain_stats(domain).live_bytes;
    if (fail_point != SIZE_MAX) fail_at = begin + fail_point;
    XrXirValueStatus status = xr_xir_view_storage_set(&view, &authority, 0, &number, &admission, &fault);
    size_t sites = value_allocation_calls - begin; fail_at = SIZE_MAX;
    CHECK(status == (fail_point == SIZE_MAX ? XR_XIR_VALUE_OK : XR_XIR_VALUE_OOM));
    if (fail_point != SIZE_MAX)
        CHECK(holder->value.payload == original && live == blocks && xr_xir_domain_stats(domain).live_bytes == bytes);
    CHECK(xir_object_references(object_pointer(&holder->value)) == -1);
    CHECK(xr_xir_view_storage_get(&view, &authority, 0, &admission, &output, &fault) == XR_XIR_VALUE_OK &&
        output.payload == (fail_point == SIZE_MAX ? 43 : 17));
    xr_xir_value_drop(&output); xr_xir_view_storage_end(&view); xr_xir_value_drop(&cell);
    xr_xir_domain_drop(domain); xr_xir_compile_type_arena_drop(arena);
    CHECK(!live && !value_compile_live);
    return sites;
}
static void signed_share_cycle_and_root_failure(void) {
    ClosedRing ring; closed_ring_new(&ring, 1, false);
    size_t attempts = value_allocation_calls;
    closed_fail_all = true;
    CHECK(xr_xir_value_share(&ring.cell) == XR_XIR_VALUE_OK);
    closed_fail_all = false;
    CHECK(value_allocation_calls == attempts && xir_object_references(object_pointer(&ring.cell)) < 0 &&
        xir_object_references(object_pointer(&ring.function)) < 0);
    xr_xir_value_drop(&ring.cell); xr_xir_value_drop(&ring.function);
    CHECK(xr_xir_domain_close(ring.domain) == XR_XIR_VALUE_OK && ring.releases == 1);
    closed_ring_owners_drop(&ring);
    CHECK(!live);
    XrXirDomain *domain = NULL; XirString *string = NULL;
    CHECK(xr_xir_root_domain_new(UINT64_MAX, &domain) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_root_string_new(domain, "pin", 3, &string) == XR_XIR_VALUE_OK);
    XrXirValue value = {XR_XIR_STRING, 0, (int64_t)(intptr_t)string};
    atomic_store(&domain->references, UINT32_MAX);
    CHECK(xr_xir_value_share(&value) == XR_XIR_VALUE_REFCOUNT_LIMIT && xir_object_references(&string->object) == 1);
    CHECK(!string->object.graph_work && !string->object.release_next);
    atomic_store(&domain->references, 1);
    CHECK(xr_xir_value_share(&value) == XR_XIR_VALUE_OK && xir_object_references(&string->object) == -1);
    xr_xir_root_domain_release(domain); xr_xir_string_release(string);
    CHECK(!live);
}
static void signed_share_second_root_pin(unsigned root_first) {
    XrXirDomain *domain = NULL;
    CHECK(xr_xir_root_domain_new(UINT64_MAX, &domain) == XR_XIR_VALUE_OK);
    XrXirTypeArena *arena = allocation_arena(domain);
    size_t releases = 0;
    XrXirValueAdmission admission = allocation_admission(domain, arena, &releases);
    admission.scratch_bytes = 65536;
    XirString *strings[2] = {NULL, NULL};
    XrXirValue cells[2] = {{0}}, function = {0};
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(xr_xir_root_string_new(domain, i ? "second" : "first", i ? 6 : 5, &strings[i]) == XR_XIR_VALUE_OK);
        XrXirValue text = {XR_XIR_STRING, 0, (int64_t)(intptr_t)strings[i]};
        XirCellNewTake prepared = {0};
        CHECK(xir_cell_new_take_prepare((XrXirType)257, &text, &admission, &prepared) == XR_XIR_VALUE_OK);
        xir_cell_new_take_commit(&prepared, &text.payload, &cells[i]);
        CHECK(!text.payload && !((XirRootString *)strings[i])->escaped);
    }
    XrXirFunctionBinding binding = {&releases, capture_release, 0, cells, 2};
    CHECK(xr_xir_function_new(domain, arena, (XrXirType)256, &binding, &admission, &function) == XR_XIR_VALUE_OK);
    uint32_t saved = atomic_load(&domain->references);
    atomic_store(&domain->references, UINT32_MAX - 1);
    CHECK(xr_xir_value_share(&function) == XR_XIR_VALUE_REFCOUNT_LIMIT);
    CHECK(atomic_load(&domain->references) == UINT32_MAX);
    unsigned escaped = 0;
    for (unsigned i = 0; i < 2; ++i) {
        escaped += ((XirRootString *)strings[i])->escaped;
        XirObject *string = &strings[i]->object, *cell = object_pointer(&cells[i]);
        CHECK(xir_object_references(string) == 1 && xir_object_references(cell) == 2);
        CHECK(!string->graph_work && !string->release_next && !cell->graph_work && !cell->release_next);
    }
    CHECK(escaped == 1 && xir_object_references(object_pointer(&function)) == 1);
    CHECK(!object_pointer(&function)->graph_work && !object_pointer(&function)->release_next);
    atomic_store(&domain->references, saved + 1);
    CHECK(xr_xir_value_share(&function) == XR_XIR_VALUE_OK);
    CHECK(atomic_load(&domain->references) == saved + 2);
    for (unsigned i = 0; i < 2; ++i)
        CHECK(((XirRootString *)strings[i])->escaped && xir_object_references(&strings[i]->object) == -1 &&
            xir_object_references(object_pointer(&cells[i])) == -2);
    if (root_first) xr_xir_root_domain_release(domain);
    xr_xir_value_drop(&function);
    for (unsigned i = 0; i < 2; ++i) xr_xir_value_drop(&cells[i]);
    if (!root_first) xr_xir_root_domain_release(domain);
    xr_xir_compile_type_arena_drop(arena);
    CHECK(releases == 1 && !live && !value_compile_live);
}
static XrXirTypeArena *signed_projection_arena(void) {
    XrXirNominalFieldIdentity record_fields[] = {{{"items",5},XR_XIR_FIELD_MUTABLE},
        {{"sentinel",8},XR_XIR_FIELD_MUTABLE}};
    XrXirNominalFieldIdentity class_fields[] = {{{"record",6},XR_XIR_FIELD_MUTABLE}};
    XrXirNominalIdentity identities[] = {
        {{"rc",2},{"Record",6},1,0,record_fields,2,XR_XIR_NOMINAL_STRUCT,NULL,0,0,{0},0},
        {{"rc",2},{"Owner",5},1,0,class_fields,1,XR_XIR_NOMINAL_CLASS,NULL,0,XR_XIR_NOMINAL_FINAL,{0},0}};
    XrXirNominalTable table = {NULL,2,identities};
    XrXirType record[] = {(XrXirType)256,XR_XIR_I64}, object[] = {(XrXirType)257};
    XrXirTypeNode nodes[] = {{.kind=XR_XIR_TYPE_ARRAY,.element=XR_XIR_I64},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={0,NULL,0,record,2}},
        {.kind=XR_XIR_TYPE_NOMINAL,.nominal={1,NULL,0,object,1}}};
    XrXirTypes types = {nodes,3,&table,NULL}; XrXirTypeArena *arena = NULL;
    CHECK(value_compile_arena(&types,65536,value_compile_limits(),&arena) == XR_XIR_VALUE_OK);
    return arena;
}
static size_t signed_projection_case(size_t point, bool bounds) {
    XrXirTypeArena *arena = signed_projection_arena(); XrXirDomain *domain = NULL;
    CHECK(xr_xir_domain_new(1048576,&domain) == XR_XIR_VALUE_OK);
    XrXirValueAdmission admission = {arena,domain,NULL,NULL,100000,65536};
    XrXirValue number = {XR_XIR_I64,0,17}, array = {0}, record = {0}, object = {0};
    CHECK(xr_xir_array_new((XrXirType)256,&number,1,&admission,&array) == XR_XIR_VALUE_OK);
    XrXirValue fields[] = {array,{XR_XIR_I64,0,42}};
    CHECK(xr_xir_struct_new((XrXirType)257,fields,2,&admission,&record) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_class_new((XrXirType)258,&record,1,&admission,&object) == XR_XIR_VALUE_OK);
    xr_xir_value_drop(&record); xr_xir_value_drop(&array);
    CHECK(xr_xir_value_share(&object) == XR_XIR_VALUE_OK);
    XirObject *identity = object_pointer(&object);
    const XrXirStorageLayout *layout = class_body_layout(identity);
    unsigned char *body = (unsigned char *)((XirClassObject *)identity+1)+layout->field_offsets[0];
    XrXirValue before = {(XrXirType)256,0,0}; memcpy(&before.payload,body,8);
    CHECK(xir_object_references(object_pointer(&before)) == -1);
    unsigned char original[16]; memcpy(original,body,sizeof(original));
    XrXirValuePathStep steps[] = {{XR_XIR_PATH_FIELD,(XrXirType)258,0},
        {XR_XIR_PATH_FIELD,(XrXirType)257,0},{XR_XIR_PATH_INDEX,(XrXirType)256,bounds ? 1 : 0}};
    XrXirValuePath path = {steps,3}; XrXirValuePlace place = {(XrXirType)258,&object.payload,NULL};
    XrXirFaultDetail fault = {0}; number.payload = 29;
    size_t begin = value_allocation_calls, blocks = live;
    uint64_t bytes = xr_xir_domain_stats(domain).live_bytes;
    if (point != SIZE_MAX) fail_at = begin + point;
    XrXirValueStatus status = xr_xir_value_path_write(&place,&path,&number,&admission,&fault);
    size_t sites = value_allocation_calls-begin; fail_at = SIZE_MAX;
    CHECK(admission.scratch_bytes == 65536);
    if (point != SIZE_MAX || bounds) {
        CHECK(status == (bounds ? XR_XIR_VALUE_BOUNDS : XR_XIR_VALUE_OOM));
        CHECK(!memcmp(original,body,sizeof(original)) && live == blocks &&
            xr_xir_domain_stats(domain).live_bytes == bytes);
    } else {
        CHECK(status == XR_XIR_VALUE_OK);
        XrXirValue after = {(XrXirType)256,0,0}; memcpy(&after.payload,body,8);
        CHECK(after.payload != before.payload && xir_object_references(object_pointer(&after)) == -1);
        XrXirValue out = {0};
        CHECK(xr_xir_array_get(&after,0,&admission,&out,&fault) == XR_XIR_VALUE_OK && out.payload == 29);
        CHECK(!memcmp(body+8,original+8,8)); xr_xir_value_drop(&out);
    }
    xr_xir_value_drop(&object); xr_xir_domain_drop(domain); xr_xir_compile_type_arena_drop(arena);
    CHECK(!live && !value_compile_live);
    return sites;
}
static void signed_rc_cases(void) {
    signed_string_bands(); signed_share_cycle_and_root_failure();
    signed_share_second_root_pin(0); signed_share_second_root_pin(1);
    puts("Owned-take two-String publication: second pin failure preserves all local bands; retry and both root retirement orders reach physical0");
    size_t view_sites = signed_shared_cell_place(SIZE_MAX);
    CHECK(view_sites);
    for (size_t i=0;i<view_sites;++i) signed_shared_cell_place(i);
    printf("Shared identity mutable-view write: %zu allocation failures preserve the actual binding\n",view_sites);
    size_t sites = signed_projection_case(SIZE_MAX,false);
    CHECK(sites);
    for (size_t i=0;i<sites;++i) signed_projection_case(i,false);
    signed_projection_case(SIZE_MAX,true);
    printf("Shared identity inline projection: %zu allocation failures preserve the original field\n",sites);
    puts("Signed object RC: local/shared limits, unique demotion, shared-owner private COW, cycles, allocation-free publication, root pin retry and physical0");
}
#endif // XIR_SIGNED_RC_CASES_H
