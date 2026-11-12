/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * register_nullable_fixture.h - Exact nested typed homes and real instructions
 */
#ifndef REGISTER_NULLABLE_FIXTURE_H
#define REGISTER_NULLABLE_FIXTURE_H
typedef struct RawNullableFixture {
    XrRbcAggregateType types[2];
    XrRbcAggregateValue homes[2];
    XrRbcAggregateAccess accesses[3];
    uint32_t code[3];
    XrRbcFunction function;
    XrRbcModule module;
} RawNullableFixture;

static void raw_nullable_fixture(RawNullableFixture *f) {
    memset(f, 0, sizeof(*f));
    XrPodLayout scalar, inner, outer;
    XR_CHECK(xr_pod_scalar_layout(XR_POD_I64, &scalar) && xr_pod_nullable_layout(scalar, &inner) &&
        xr_pod_nullable_layout(inner, &outer), "nullable fixture has checked scalar and aggregate layouts");
    f->types[0] = (XrRbcAggregateType){XR_RBC_NULLABLE, 0, XR_POD_I64, inner};
    f->types[1] = (XrRbcAggregateType){XR_RBC_NULLABLE, 0, XR_POD_SCALAR_COUNT, outer};
    f->homes[0] = (XrRbcAggregateValue){0, 0};
    f->homes[1] = (XrRbcAggregateValue){1, inner.size};
    f->accesses[0] = (XrRbcAggregateAccess){.operation=XR_RBC_NULLABLE_SOME, .result={0, false}};
    f->accesses[1] = (XrRbcAggregateAccess){.operation=XR_RBC_NULLABLE_SOME, .destination=1};
    f->accesses[2] = (XrRbcAggregateAccess){.operation=XR_RBC_AGGREGATE_RETURN, .source=1};
    for (uint32_t n = 0; n < 3; ++n) immediate(f->code, n, XR_RBC_AGGREGATE_ACCESS, 0, n);
    f->function = (XrRbcFunction){.code=f->code, .code_count=3, .register_count=1, .parameter_count=1,
        .aggregate_values=f->homes, .aggregate_value_count=2, .aggregate_size=inner.size + outer.size,
        .aggregate_accesses=f->accesses, .aggregate_access_count=3, .aggregate_result_type=2};
    f->module = (XrRbcModule){.functions=&f->function, .function_count=1,
        .aggregate_types=f->types, .aggregate_type_count=2};
}
#endif // REGISTER_NULLABLE_FIXTURE_H
