/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsemver.h - Semantic versioning parser
 *
 * KEY CONCEPT:
 *   Parse and compare semantic versions (MAJOR.MINOR.PATCH[-PRERELEASE][+BUILD]).
 *   Parse version constraints (^1.0.0, ~1.2.0, >=1.0.0, etc).
 *
 * CONSTRAINT SYNTAX:
 *   ^1.2.3  - Compatible (1.2.3 <= v < 2.0.0)
 *   ~1.2.3  - Patch-level (1.2.3 <= v < 1.3.0)
 *   >=, <=, >, <, = - Comparisons
 *   *       - Any version
 */

#ifndef XSEMVER_H
#define XSEMVER_H

#include <stdbool.h>
#include "../base/xdefs.h"

typedef struct XrSemVer {
    int major;
    int minor;
    int patch;
    char *prerelease;       // e.g. alpha, beta.1
    char *build;            // e.g. build.123
} XrSemVer;

typedef enum {
    SEMVER_OP_EQ,           // =
    SEMVER_OP_GT,           // >
    SEMVER_OP_GE,           // >=
    SEMVER_OP_LT,           // <
    SEMVER_OP_LE,           // <=
    SEMVER_OP_CARET,        // ^ compatible
    SEMVER_OP_TILDE,        // ~ patch-level
    SEMVER_OP_ANY           // *
} XrSemVerOp;

typedef struct XrVersionConstraint {
    XrSemVerOp op;
    XrSemVer version;
} XrVersionConstraint;

/* ========== Version API ========== */

XR_FUNC bool xr_semver_parse(const char *str, XrSemVer *ver);
XR_FUNC void xr_semver_free(XrSemVer *ver);

// Returns <0 if a<b, 0 if a==b, >0 if a>b
// Prerelease versions are less than release versions
XR_FUNC int xr_semver_compare(const XrSemVer *a, const XrSemVer *b);

XR_FUNC int xr_semver_to_string(const XrSemVer *ver, char *buf, int size);

/* ========== Constraint API ========== */

XR_FUNC bool xr_constraint_parse(const char *str, XrVersionConstraint *constraint);
XR_FUNC void xr_constraint_free(XrVersionConstraint *constraint);
XR_FUNC bool xr_constraint_matches(const XrSemVer *ver, const XrVersionConstraint *constraint);
XR_FUNC int xr_constraint_to_string(const XrVersionConstraint *constraint, char *buf, int size);

/* ========== Utility ========== */

XR_FUNC bool xr_semver_is_valid(const char *str);

// Returns index of best matching version, or -1 if none match
XR_FUNC int xr_semver_select_best(const XrSemVer *versions, int count, 
                          const XrVersionConstraint *constraint);

#endif // XSEMVER_H
