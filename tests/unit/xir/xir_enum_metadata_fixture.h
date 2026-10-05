/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_enum_metadata_fixture.h - Ordered variants with independently named payloads
 */
#ifndef XIR_ENUM_METADATA_FIXTURE_H
#define XIR_ENUM_METADATA_FIXTURE_H
#include "xir/xxir_nominal.h"
typedef struct EnumMetadataFixture {
    char names[3][6];
    XrXirNominalVariant variants[3];
    XrXirNominalField fields[2];
    XrXirNominalDeclaration declaration;
    XrXirNominalTable table;
} EnumMetadataFixture;
static inline void enum_metadata_fixture(EnumMetadataFixture *f) {
    memset(f, 0, sizeof(*f));
    memcpy(f->names[0], "None", 4); memcpy(f->names[1], "Left", 4); memcpy(f->names[2], "Right", 5);
    f->variants[0] = (XrXirNominalVariant) {{f->names[0], 4}, 0, 0};
    f->variants[1] = (XrXirNominalVariant) {{f->names[1], 4}, 0, 1};
    f->variants[2] = (XrXirNominalVariant) {{f->names[2], 5}, 1, 1};
    f->fields[0] = (XrXirNominalField) {{"value", 5}, XR_XIR_I64, 0};
    f->fields[1] = (XrXirNominalField) {{"value", 5}, XR_XIR_STRING, 0};
    f->declaration = (XrXirNominalDeclaration) {{"alpha", 5}, {"Choice", 6}, 1, NULL, 0,
        f->fields, 2, XR_XIR_NOMINAL_ENUM, f->variants, 3, 0,{0}};
    f->table = (XrXirNominalTable) {&f->declaration, 1, NULL};
}
#endif // XIR_ENUM_METADATA_FIXTURE_H
