/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_analyzer_enum_storage.c - Real enum policy, fault and lifetime boundaries
 */
#include "frontend/analyzer/xanalyzer_enum_storage.h"
#include "frontend/parser/xast_nodes_common.h"
#include "runtime/value/xtype_pool.h"
#include "toolchain/xcompiler_session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "%d: %s\n", __LINE__, #c);                                             \
            exit(1);                                                                               \
        }                                                                                          \
    } while (0)
#define xr_compile_resources_work xr_test_actual_resources_work
#include "../base/parser_resources_observer.h"
#undef xr_compile_resources_work
static bool recording;
static uint64_t boundaries[65536];
static size_t boundary_count, work_calls;
XR_FUNC XrCompileResourceStatus xr_compile_resources_work(XrCompileResources *resources,
                                                          uint64_t units) {
    ++work_calls;
    XrCompileResourceStatus status = xr_test_actual_resources_work(resources, units);
    if (recording && status == XR_COMPILE_RESOURCE_OK && units) {
        XrCompileResourceStats now = {0};
        CHECK(xr_compile_resources_stats(resources, &now) == XR_COMPILE_RESOURCE_OK);
        CHECK(boundary_count < sizeof(boundaries) / sizeof(boundaries[0]));
        boundaries[boundary_count++] = now.work;
    }
    return status;
}
static const XrCompileResourceLimits ample = {1048576, 1048576, 1048576};
typedef struct Fixture {
    XrCompileResources *resources;
    XrCompilerSession *session;
    XrCompileState *state;
    XrTypePool *pool;
    XrTypePoolPin pin;
    XrType *types[2];
    XrOsIoPolicy policy;
    XaEnumInfo *info, *copy;
    XrEnumLayout *layout;
    bool root_closed, creator_closed;
} Fixture;
static XrCompileResourceStats stats(XrCompileResources *resources) {
    XrCompileResourceStats result = {0};
    CHECK(xr_compile_resources_stats(resources, &result) == XR_COMPILE_RESOURCE_OK);
    return result;
}
#define TRY(c)                                                                                     \
    do {                                                                                           \
        XrCompileResourceStatus status_ = (c);                                                     \
        if (status_ != XR_COMPILE_RESOURCE_OK)                                                     \
            return status_;                                                                        \
    } while (0)
#define IO(c) TRY(xa_enum_capture_status(fixture->state, (c)))
static XrCompileResourceStatus open_fixture(Fixture *fixture, bool dead,
                                            const XrCompileResourceLimits *limits) {
    TRY(xr_compile_resources_new(limits, &fixture->resources));
    XrCompilerSessionStatus opened = xr_compile_session_new(fixture->resources, &fixture->session);
    CHECK(opened == XR_COMPILER_SESSION_OK || opened == XR_COMPILER_SESSION_BUDGET ||
          opened == XR_COMPILER_SESSION_OUT_OF_MEMORY);
    if (opened != XR_COMPILER_SESSION_OK)
        return opened == XR_COMPILER_SESSION_BUDGET ? XR_COMPILE_RESOURCE_BUDGET
                                                    : XR_COMPILE_RESOURCE_OUT_OF_MEMORY;
    fixture->state = xr_compile_session_compile_state(fixture->session);
    TRY(xr_compile_type_pool_open(fixture->state, &fixture->pool));
    TRY(xr_type_pool_pin(fixture->state, fixture->pool, &fixture->pin));
    fixture->types[0] = xr_pool_alloc_type(fixture->pool, XR_KIND_INT);
    fixture->types[1] = xr_pool_alloc_type(fixture->pool, XR_KIND_STRING);
    TRY(xr_compile_state_status(fixture->state));
    CHECK(fixture->types[0] && fixture->types[1] && fixture->types[0]->id == 1 &&
          fixture->types[1]->id == 2);
    fixture->policy = xr_compile_io_policy(fixture->resources);
    if (dead) {
        xr_type_pool_free(fixture->pool);
        fixture->creator_closed = true;
        xr_compile_session_free(fixture->session);
        fixture->session = NULL;
        xr_compile_resources_release(fixture->resources);
        fixture->root_closed = true;
    }
    return XR_COMPILE_RESOURCE_OK;
}
static void check_info(const Fixture *fixture, const XaEnumInfo *info) {
    CHECK(info && !strcmp(info->nominal_owner, "module:/enum-fi") && !strcmp(info->name, "Packet"));
    CHECK(info->variant_count == 3 && info->is_payload_enum && info->layout);
    const XaEnumVariantInfo *variant = &info->variants[0];
    CHECK(variant->owner == info && !strcmp(variant->name, "Data") && variant->tag == 0 &&
          variant->symbol == 71);
    CHECK(variant->payload_count == 2 && !strcmp(variant->payload_names[0], "code") &&
          !strcmp(variant->payload_names[1], "text"));
    CHECK(variant->payload_types[0] == fixture->types[0] &&
          variant->payload_types[1] == fixture->types[1]);
    CHECK(!strcmp(variant->payload_locations[0].file, "input.xr") &&
          variant->payload_locations[0].line == 3);
    CHECK(variant->payload_locations[0].column == 5 &&
          variant->payload_locations[0].end_column == 9);
    CHECK(variant->payload_locations[1].column == 14 &&
          variant->payload_locations[1].end_column == 18);
    CHECK(!strcmp(info->variants[1].name, "End") &&
          !strcmp(info->variants[2].name, "Unicode_\xE4\xB8\xAD"));
    CHECK(info->layout->variant_count == 3 && info->layout->layout_id != 0 &&
          info->layout->variants[0].payload_count == 2);
    CHECK(info->layout->variants[0].payload_type_ids[0] == 8 &&
          info->layout->variants[0].payload_type_ids[1] == XR_TID_STRING);
    CHECK(!strcmp(info->layout->variants[0].payload_names[0], "code") &&
          !strcmp(info->layout->variants[0].name, "Data"));
    CHECK(info->layout->layout_id == xr_enum_layout_nominal_id(info->layout));
}
static XrCompileResourceStatus finalize(Fixture *fixture) {
    XaEnumInfo old = *fixture->info;
    XaEnumVariantInfo variants[3];
    memcpy(variants, old.variants, sizeof(variants));
    XrEnumLayout header = {0};
    XrEnumVariantLayout rows[3];
    if (old.layout) {
        header = *old.layout;
        memcpy(rows, header.variants, sizeof(rows));
    }
    XrOsIoStatus result = xa_enum_info_finalize_layout(fixture->info);
    if (result != XR_OS_IO_OK) {
        CHECK(!memcmp(&old, fixture->info, sizeof(old)) &&
              !memcmp(variants, old.variants, sizeof(variants)));
        if (old.layout) {
            CHECK(!memcmp(&header, old.layout, sizeof(header)) &&
                  !memcmp(rows, header.variants, sizeof(rows)));
            check_info(fixture, fixture->info);
        }
    }
    return xa_enum_capture_status(fixture->state, result);
}
static XrCompileResourceStatus produce_info(Fixture *fixture) {
    char owner[] = "module:/enum-fi", name[] = "Packet";
    IO(xa_enum_info_new(&fixture->policy, owner, name, 3, &fixture->info));
    memset(owner, '!', sizeof(owner) - 1);
    memset(name, '!', sizeof(name) - 1);
    const char *names[] = {"Data", "End", "Unicode_\xE4\xB8\xAD"};
    for (unsigned i = 0; i < 3; ++i) {
        char input[40];
        snprintf(input, sizeof(input), "%s", names[i]);
        IO(xa_enum_variant_set_name(&fixture->info->variants[i], input));
        memset(input, '!', strlen(input));
    }
    XaEnumVariantInfo *variant = &fixture->info->variants[0];
    XaEnumVariantInfo old_variant = *variant;
    XrOsIoStatus renamed = xa_enum_variant_set_name(variant, variant->name);
    if (renamed != XR_OS_IO_OK)
        CHECK(!memcmp(&old_variant, variant, sizeof(old_variant)));
    IO(renamed);
    variant->payload_count = 2;
    variant->symbol = 71;
    char first[] = "code", second[] = "text", file[] = "input.xr";
    const char *fields[] = {first, second};
    IO(xa_enum_variant_set_payload_names(variant, fields, 2));
    memset(first, '!', 4);
    memset(second, '!', 4);
    const XrNameSpan spans[] = {{.line = 3, .column = 5}, {.line = 3, .column = 14}};
    IO(xa_enum_variant_set_payload_locations(variant, file, spans, 2));
    memset(file, '!', sizeof(file) - 1);
    IO(xa_enum_variant_set_payload_types(variant, fixture->types, 2));
    TRY(finalize(fixture));
    check_info(fixture, fixture->info);
    TRY(finalize(fixture));
    check_info(fixture, fixture->info);
    IO(xa_enum_info_clone(&fixture->policy, fixture->info, &fixture->copy));
    check_info(fixture, fixture->copy);
    CHECK(fixture->copy != fixture->info && fixture->copy->layout != fixture->info->layout);
    CHECK(fixture->copy->variants[0].payload_names != fixture->info->variants[0].payload_names);
    CHECK(fixture->copy->variants[0].payload_locations[0].file !=
          fixture->info->variants[0].payload_locations[0].file);
    CHECK(fixture->copy->variants[0].name != fixture->info->variants[0].name);
    xa_enum_info_free(fixture->info);
    fixture->info = NULL;
    check_info(fixture, fixture->copy);
    return XR_COMPILE_RESOURCE_OK;
}
static XrCompileResourceStatus produce_layout(Fixture *fixture) {
    char owner[] = "module:/layout-fi", name[] = "Value", a[] = "Data", b[] = "End";
    const char *names[] = {a, b};
    IO(xr_enum_layout_new(&fixture->policy, owner, name, names, 2, &fixture->layout));
    memset(owner, '!', sizeof(owner) - 1);
    memset(name, '!', sizeof(name) - 1);
    memset(a, '!', 4);
    memset(b, '!', 3);
    XrEnumLayout *layout = fixture->layout;
    CHECK(!strcmp(layout->nominal_owner, "module:/layout-fi") && !strcmp(layout->name, "Value") &&
          !strcmp(layout->variants[0].name, "Data"));
    int counts[] = {2, 0};
    IO(xr_enum_layout_set_payload_counts(layout, counts, 2));
    const char *fields[] = {"code", "text"};
    uint8_t ids[] = {8, XR_TID_STRING};
    IO(xr_enum_layout_set_variant_payload_metadata(layout, 0, fields, ids, 2));
    IO(xr_enum_layout_set_variant_symbol(layout, 0, 99));
    CHECK(layout->variants[0].symbol == 99);
    XrEnumLayout before = *layout;
    XrEnumVariantLayout rows[2];
    memcpy(rows, layout->variants, sizeof(rows));
    XrOsIoStatus copied = xr_enum_layout_set_variant_payload_metadata(
        layout, 0, layout->variants[0].payload_names, layout->variants[0].payload_type_ids, 2);
    if (copied != XR_OS_IO_OK)
        CHECK(!memcmp(&before, layout, sizeof(before)) &&
              !memcmp(rows, layout->variants, sizeof(rows)));
    IO(copied);
    CHECK(!strcmp(layout->variants[0].payload_names[0], "code") &&
          layout->variants[0].payload_type_ids[0] == 8);
    before = *layout;
    memcpy(rows, layout->variants, sizeof(rows));
    int zero[] = {0, 0};
    XrOsIoStatus changed = xr_enum_layout_set_payload_counts(layout, zero, 2);
    if (changed != XR_OS_IO_OK) {
        CHECK(!memcmp(&before, layout, sizeof(before)) &&
              !memcmp(rows, layout->variants, sizeof(rows)));
        CHECK(!strcmp(layout->variants[0].payload_names[0], "code") &&
              layout->variants[0].payload_type_ids[1] == XR_TID_STRING);
    }
    IO(changed);
    CHECK(layout->is_zero_payload && !layout->variants[0].payload_names &&
          !layout->variants[0].payload_type_ids);
    CHECK(layout->layout_id == xr_enum_layout_nominal_id(layout) &&
          layout->layout_id != before.layout_id);
    return XR_COMPILE_RESOURCE_OK;
}
static XrCompileResourceStatus pipeline(bool dead, bool observe,
                                        const XrCompileResourceLimits *limits,
                                        XrCompileResourceStats *observed) {
    Fixture fixture = {0};
    recording = observe;
    XrCompileResourceStatus status = open_fixture(&fixture, dead, limits);
    if (status == XR_COMPILE_RESOURCE_OK)
        status = produce_info(&fixture);
    if (status == XR_COMPILE_RESOURCE_OK)
        status = produce_layout(&fixture);
    if (status != XR_COMPILE_RESOURCE_OK && fixture.state) {
        size_t calls = work_calls, attempts = attempt_count;
        CHECK(xa_enum_capture_status(fixture.state, XR_OS_IO_BAD_ARGUMENT) == status);
        CHECK(calls == work_calls && attempts == attempt_count);
    }
    if (fixture.resources)
        *observed = stats(fixture.resources);
    size_t calls = work_calls, attempts = attempt_count;
    xa_enum_info_free(fixture.info);
    xa_enum_info_free(fixture.copy);
    xr_enum_layout_free(fixture.layout);
    if (!fixture.creator_closed)
        xr_type_pool_free(fixture.pool);
    xr_type_pool_unpin(&fixture.pin);
    xr_compile_session_free(fixture.session);
    if (!fixture.root_closed)
        xr_compile_resources_release(fixture.resources);
    CHECK(calls == work_calls && attempts == attempt_count && !physical_live && !allocation_count);
    recording = false;
    return status;
}
static void matrix(bool dead) {
    reset();
    boundary_count = 0;
    XrCompileResourceStats baseline = {0};
    CHECK(pipeline(dead, true, &ample, &baseline) == XR_COMPILE_RESOURCE_OK);
    size_t sites = attempt_count, cuts = boundary_count;
    CHECK(sites && cuts);
    XrCompileResourceLimits exact = {baseline.allocated_bytes, baseline.peak_bytes, baseline.work};
    for (unsigned axis = 0; axis < 3; ++axis)
        for (int delta = -1; delta <= 1; ++delta) {
            XrCompileResourceLimits limits = exact;
            uint64_t *limit = axis == 0   ? &limits.allocated_bytes
                              : axis == 1 ? &limits.live_bytes
                                          : &limits.work;
            *limit = (uint64_t) ((int64_t) *limit + delta);
            reset();
            XrCompileResourceStats observed = {0};
            CHECK(pipeline(dead, false, &limits, &observed) ==
                  (delta < 0 ? XR_COMPILE_RESOURCE_BUDGET : XR_COMPILE_RESOURCE_OK));
        }
    for (size_t i = 0; i < sites; ++i) {
        reset();
        fail_at = i;
        XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, false, &ample, &observed) == XR_COMPILE_RESOURCE_OUT_OF_MEMORY);
    }
    for (size_t i = 0; i < cuts; ++i) {
        reset();
        XrCompileResourceLimits limits = ample;
        limits.work = boundaries[i] - 1;
        XrCompileResourceStats observed = {0};
        CHECK(pipeline(dead, false, &limits, &observed) == XR_COMPILE_RESOURCE_BUDGET);
    }
    printf("enum storage dead%d: %zu actual allocation faults/%zu observed work cuts; "
           "oldlayout/clone/output,axes/adapter-firstcause/physical0 PASS\n",
           dead, sites, cuts);
}
static void guards(void) {
    reset();
    Fixture fixture = {0};
    CHECK(open_fixture(&fixture, false, &ample) == XR_COMPILE_RESOURCE_OK);
    CHECK(produce_info(&fixture) == XR_COMPILE_RESOURCE_OK);
    XaEnumInfo *info = fixture.copy;
    XaEnumInfo *occupied = info;
    size_t calls = work_calls, attempts = attempt_count;
    CHECK(xa_enum_info_clone(&fixture.policy, (const XaEnumInfo *) (uintptr_t) 1, &occupied) ==
              XR_OS_IO_BAD_ARGUMENT &&
          occupied == info);
    CHECK(xa_enum_info_new(&fixture.policy, "owner", "Name", 2, &occupied) ==
              XR_OS_IO_BAD_ARGUMENT &&
          occupied == info);
    XaEnumVariantInfo copy = info->variants[0];
    CHECK(xa_enum_variant_set_name(&copy, "copy") == XR_OS_IO_BAD_ARGUMENT);
    CHECK(calls == work_calls && attempts == attempt_count &&
          !strcmp(info->variants[0].name, "Data"));
    uint32_t count = info->variant_count;
    info->variant_count = count + 1;
    XaEnumInfo *out = NULL;
    CHECK(xa_enum_info_clone(&fixture.policy, info, &out) == XR_OS_IO_BAD_ARGUMENT && !out);
    info->variant_count = count;
    XaEnumVariantInfo *variant = &info->variants[0];
    uint16_t payload_count = variant->payload_count;
    variant->payload_count = 3;
    CHECK(xa_enum_info_clone(&fixture.policy, info, &out) == XR_OS_IO_BAD_ARGUMENT && !out);
    CHECK(xa_enum_info_finalize_layout(info) == XR_OS_IO_BAD_ARGUMENT);
    variant->payload_count = payload_count;
    check_info(&fixture, info);
    const char *names[] = {"Only"};
    XrEnumLayout *layout = NULL;
    CHECK(xr_enum_layout_new(&fixture.policy, "owner", "Name", names, 1, &layout) == XR_OS_IO_OK);
    int counts[] = {2};
    CHECK(xr_enum_layout_set_payload_counts(layout, counts, 1) == XR_OS_IO_OK);
    const char *duplicate[] = {"same", "same"};
    uint8_t ids[] = {8, 8};
    XrEnumLayout header = *layout;
    XrEnumVariantLayout row = layout->variants[0];
    CHECK(xr_enum_layout_set_variant_payload_metadata(layout, 0, duplicate, ids, 2) ==
          XR_OS_IO_BAD_ARGUMENT);
    CHECK(!memcmp(&header, layout, sizeof(header)) &&
          !memcmp(&row, &layout->variants[0], sizeof(row)));
    counts[0] = 65536;
    CHECK(xr_enum_layout_set_payload_counts(layout, counts, 1) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(!memcmp(&header, layout, sizeof(header)) &&
          !memcmp(&row, &layout->variants[0], sizeof(row)));
    counts[0] = -1;
    CHECK(xr_enum_layout_set_payload_counts(layout, counts, 1) == XR_OS_IO_BAD_ARGUMENT);
    CHECK(xr_compile_state_status(fixture.state) == XR_COMPILE_RESOURCE_OK);
    calls = work_calls;
    attempts = attempt_count;
    xr_enum_layout_free(layout);
    info->variants[0].payload_count = 65535;
    info->variant_count = 0;
    xa_enum_info_free(info);
    fixture.copy = NULL;
    xr_type_pool_free(fixture.pool);
    xr_type_pool_unpin(&fixture.pin);
    xr_compile_session_free(fixture.session);
    xr_compile_resources_release(fixture.resources);
    CHECK(calls == work_calls && attempts == attempt_count && !physical_live && !allocation_count);
    puts("enum storage guards: occupied/copy/shape/count,duplicate/overflow semantic "
         "rejection,owned cleanup PASS");
}
static void receiving_policy(void) {
    reset();
    Fixture source = {0}, target = {0};
    CHECK(open_fixture(&source, false, &ample) == XR_COMPILE_RESOURCE_OK);
    CHECK(produce_info(&source) == XR_COMPILE_RESOURCE_OK);
    CHECK(open_fixture(&target, false, &ample) == XR_COMPILE_RESOURCE_OK);
    XrCompileResourceStats before = stats(source.resources);
    CHECK(xa_enum_info_clone(&target.policy, source.copy, &target.copy) == XR_OS_IO_OK);
    XrCompileResourceStats after = stats(source.resources);
    CHECK(before.work == after.work && before.allocated_bytes == after.allocated_bytes &&
          before.live_bytes == after.live_bytes);
    check_info(&source, target.copy);
    CHECK(target.copy->variants[0].payload_types[0] == source.types[0]);
    size_t calls = work_calls, attempts = attempt_count;
    xa_enum_info_free(source.copy);
    source.copy = NULL;
    xr_type_pool_free(source.pool);
    xr_compile_session_free(source.session);
    xr_compile_resources_release(source.resources);
    xr_type_pool_free(target.pool);
    xr_type_pool_unpin(&target.pin);
    xr_compile_session_free(target.session);
    xr_compile_resources_release(target.resources);
    /* The real source pool pin owns the borrowed payload types. Metadata blocks
     * alone keep the receiving ledger alive after its state/session die. */
    check_info(&source, target.copy);
    CHECK(stats(target.resources).live_bytes != 0);
    xa_enum_info_free(target.copy);
    xr_type_pool_unpin(&source.pin);
    CHECK(calls == work_calls && attempts == attempt_count && !physical_live && !allocation_count);
    puts("enum receiving policy: source ledger unchanged, receiving owner/session dead, real type "
         "pin held, physical0 PASS");
}
int main(void) {
    matrix(false);
    matrix(true);
    guards();
    receiving_policy();
    return 0;
}
