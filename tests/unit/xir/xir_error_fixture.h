/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_error_fixture.h - Typed enum errors shared by execution witnesses
 */
#ifndef XIR_ERROR_FIXTURE_H
#define XIR_ERROR_FIXTURE_H
#include "xir/xxir_nominal.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_enum.h"
typedef struct ErrorFixture {
    XrXirNominalVariant variants[2];
    XrXirNominalField fields[2];
    XrXirNominalFieldIdentity field_ids[2];
    XrXirNominalDeclaration declaration;
    XrXirNominalIdentity identity;
    XrXirNominalTable table;
    XrXirType field_types[2];
    XrXirTypeNode node;
    XrXirTypes types;
} ErrorFixture;
static inline void error_fixture_init(ErrorFixture *f, bool runtime) {
    memset(f, 0, sizeof(*f));
    f->variants[0] = (XrXirNominalVariant) {{"Code",4},0,1};
    f->variants[1] = (XrXirNominalVariant) {{"Text",4},1,1};
    f->fields[0] = (XrXirNominalField) {{"code",4},XR_XIR_I64,0};
    f->fields[1] = (XrXirNominalField) {{"text",4},XR_XIR_STRING,0};
    f->field_ids[0] = (XrXirNominalFieldIdentity) {{"code",4},0};
    f->field_ids[1] = (XrXirNominalFieldIdentity) {{"text",4},0};
    f->declaration = (XrXirNominalDeclaration) {{"alpha",5},{"Failure",7},1,NULL,0,
        f->fields,2,XR_XIR_NOMINAL_ENUM,f->variants,2, 0};
    f->identity = (XrXirNominalIdentity) {{"alpha",5},{"Failure",7},1,0,
        f->field_ids,2,XR_XIR_NOMINAL_ENUM,f->variants,2, 0};
    f->table = (XrXirNominalTable) {runtime ? NULL : &f->declaration,1,runtime ? &f->identity : NULL};
    f->field_types[0] = XR_XIR_I64; f->field_types[1] = XR_XIR_STRING;
    f->node.kind = XR_XIR_TYPE_NOMINAL;
    if (runtime) { f->node.nominal.fields=f->field_types; f->node.nominal.field_count=2; }
    f->types = (XrXirTypes) {&f->node,1,&f->table, NULL};
}
static inline XrXirValueStatus error_fixture_arena(const XrXirCompileContext *context, XrXirTypeArena **output) {
    ErrorFixture f; error_fixture_init(&f,true);
    XrXirValueStatus status=xr_xir_compile_type_arena_new(context,&f.types,output);
    memset(&f,0xCC,sizeof(f)); return status;
}
static inline XrXirValueAdmission error_fixture_admission(XrXirDomain *domain, XrXirTypeArena *arena) {
    return (XrXirValueAdmission) {arena,domain,NULL,NULL,1000000,65536};
}
static inline XrXirValue error_fixture_code(XrXirValueAdmission *admission, int64_t code) {
    XrXirValue field={XR_XIR_I64,0,code}, value={0};
    CHECK(xr_xir_enum_new((XrXirType)256,0,&field,1,admission,&value)==XR_XIR_VALUE_OK);
    return value;
}
static inline bool error_fixture_is_code(const XrXirValue *value, XrXirDomain *domain, int64_t code) {
    XrXirValueAdmission admission={xr_xir_value_arena(value),domain,NULL,NULL,10000,65536};
    XrXirValue field={0};
    bool ok=xr_xir_enum_get(value,0,0,&admission,&field)==XR_XIR_VALUE_OK &&
        field.type==XR_XIR_I64 && field.payload==code;
    xr_xir_value_drop(&field); return ok;
}
#endif // XIR_ERROR_FIXTURE_H
