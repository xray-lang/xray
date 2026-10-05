/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_nominal_fixture.h - Mutable producer-owned nominal metadata fixtures
 *
 * KEY CONCEPT:
 *   Poisoned producer buffers cannot affect owned declaration snapshots.
 */
#ifndef XIR_NOMINAL_FIXTURE_H
#define XIR_NOMINAL_FIXTURE_H
#include "xir/xxir_nominal.h"
typedef struct NominalFixture {
    char module[5], name[4], field[5];
    XrXirConstraint constraint;
    XrXirNominalField fields[2];
    XrXirNominalDeclaration declarations[2];
    XrXirNominalTable table;
} NominalFixture;
static inline void nominal_fixture(NominalFixture *f) {
    memset(f, 0, sizeof(*f));
    memcpy(f->module, "alpha", 5); memcpy(f->name, "Pair", 4); memcpy(f->field, "value", 5);
    f->constraint.markers = XR_XIR_CONSTRAINT_SENDABLE;
    f->fields[0] = (XrXirNominalField) {{f->field, 5}, (XrXirType) XR_XIR_TYPE_PARAMETER_BASE, XR_XIR_FIELD_MUTABLE};
    f->fields[1] = (XrXirNominalField) {{"label", 5}, XR_XIR_STRING, XR_XIR_FIELD_PRIVATE};
    f->declarations[0] = (XrXirNominalDeclaration) {{f->module, 5}, {f->name, 4}, 1,
        &f->constraint, 1, f->fields, 2, XR_XIR_NOMINAL_STRUCT, NULL, 0, 0, {0}};
    f->declarations[1] = f->declarations[0];
    f->declarations[1].module = (XrXirLiteral) {"other", 5};
    f->table = (XrXirNominalTable) {f->declarations, 2, NULL};
}
typedef struct NominalIdentityFixture {
    char module[6], name[5], field[6];
    XrXirNominalFieldIdentity fields[2];
    XrXirNominalIdentity identities[2];
    XrXirNominalTable table;
} NominalIdentityFixture;
static inline void nominal_identity_fixture(NominalIdentityFixture *f) {
    memset(f, 0, sizeof(*f));
    memcpy(f->module, "alpha", 5); memcpy(f->name, "Pair", 4); memcpy(f->field, "value", 5);
    f->fields[0] = (XrXirNominalFieldIdentity) {{f->field, 5}, XR_XIR_FIELD_MUTABLE};
    f->fields[1] = (XrXirNominalFieldIdentity) {{"label", 5}, XR_XIR_FIELD_PRIVATE};
    f->identities[0] = (XrXirNominalIdentity) {{f->module, 5}, {f->name, 4}, 1, 0, f->fields, 2, XR_XIR_NOMINAL_STRUCT, NULL, 0, 0, {0}};
    f->identities[1] = f->identities[0]; f->identities[1].module = (XrXirLiteral) {"other", 5};
    f->table = (XrXirNominalTable) {NULL, 2, f->identities};
}
#endif // XIR_NOMINAL_FIXTURE_H
