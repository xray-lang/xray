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
#include "../base/xio_policy.h"

typedef struct XrSemVer {
    XrOsIoPolicy policy;  // Owns optional suffixes; copied by successful parsing.
    int major;
    int minor;
    int patch;
    char *prerelease;  // e.g. alpha, beta.1
    char *build;       // e.g. build.123
} XrSemVer;

typedef enum {
    SEMVER_OP_EQ,     // =
    SEMVER_OP_GT,     // >
    SEMVER_OP_GE,     // >=
    SEMVER_OP_LT,     // <
    SEMVER_OP_LE,     // <=
    SEMVER_OP_CARET,  // ^ compatible
    SEMVER_OP_TILDE,  // ~ patch-level
    SEMVER_OP_ANY     // *
} XrSemVerOp;

typedef struct XrVersionConstraint {
    XrSemVerOp op;
    XrSemVer version;
} XrVersionConstraint;

/* Parsing has one policy-bearing algorithm. Invalid syntax returns
 * BAD_ARGUMENT without publishing output; the validator instead publishes
 * OK/false for invalid syntax. Resource failures never become invalid syntax.
 * Every destructor releases through the policy that produced the value. */
XR_FUNC XrOsIoStatus xr_semver_parse_owned(const XrOsIoPolicy *policy, const char *text, XrSemVer *output);
XR_FUNC void xr_semver_free_owned(XrSemVer *version);
XR_FUNC XrOsIoStatus xr_semver_compare_owned(const XrOsIoPolicy *policy, const XrSemVer *left,
    const XrSemVer *right, int *output);
XR_FUNC XrOsIoStatus xr_semver_to_string_owned(const XrOsIoPolicy *policy, const XrSemVer *version,
    char *buffer, size_t capacity, size_t *written);
XR_FUNC XrOsIoStatus xr_constraint_parse_owned(const XrOsIoPolicy *policy, const char *text,
    XrVersionConstraint *output);
XR_FUNC void xr_constraint_free_owned(XrVersionConstraint *constraint);
XR_FUNC XrOsIoStatus xr_constraint_matches_owned(const XrOsIoPolicy *policy, const XrSemVer *version,
    const XrVersionConstraint *constraint, bool *output);
XR_FUNC XrOsIoStatus xr_constraint_to_string_owned(const XrOsIoPolicy *policy, const XrVersionConstraint *constraint,
    char *buffer, size_t capacity, size_t *written);
XR_FUNC XrOsIoStatus xr_semver_is_valid_owned(const XrOsIoPolicy *policy, const char *text, bool *output);
/* OK publishes the matching index, or -1 when no version matches. */
XR_FUNC XrOsIoStatus xr_semver_select_best_owned(const XrOsIoPolicy *policy, const XrSemVer *versions,
    int count, const XrVersionConstraint *constraint, int *output);

#endif  // XSEMVER_H
