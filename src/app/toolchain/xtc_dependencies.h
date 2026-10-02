/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_dependencies.h - Owned facts from bounded native dependency reports
 */
#ifndef XTC_DEPENDENCIES_H
#define XTC_DEPENDENCIES_H
#include "xtc_xir_target.h"

typedef enum XrDependencyFormat {
    XR_DEPENDENCY_MSVC_SOURCE_1_2,
    XR_DEPENDENCY_WINDOWS_MAKE,
    XR_DEPENDENCY_MSVC_LIBRARY_ZH_CN,
    XR_DEPENDENCY_MSVC_FULLPATH_RSP_UTF16LE
} XrDependencyFormat;
typedef enum XrDependencyKind {
    XR_DEPENDENCY_SOURCE, XR_DEPENDENCY_HEADER, XR_DEPENDENCY_LIBRARY_SEARCH,
    XR_DEPENDENCY_LINK_INPUT
} XrDependencyKind;
typedef enum XrDependencyResponseFacts {
    XR_DEPENDENCY_NO_REFERENCE_IN_CAPTURED_ARGV
} XrDependencyResponseFacts;
typedef struct XrDependencyInput {
    XrDependencyFormat format;
    const uint8_t *bytes;
    size_t length;
    const char *const *argv;
    uint32_t argc;
} XrDependencyInput;
typedef struct XrDependencyLimits {
    size_t frame_bytes, path_bytes;
    uint32_t records;
} XrDependencyLimits;
typedef struct XrDependencyRecord {
    const char *path;
    size_t offset;
    XrDependencyKind kind;
} XrDependencyRecord;
typedef struct XrDependencyFacts XrDependencyFacts;

/* No I/O, implicit cwd, role inference, deduplication or completeness claim.
 * Inputs are copied; paths preserve spelling and occurrence order. Any '@'
 * in captured argv is unsupported. This says nothing about internal response
 * files. Limits constrain shape; all actual allocation and work use resources.
 * FULLPATH_RSP accepts a UTF-16LE BOM and quoted absolute paths, each followed
 * by CRLF. Options and response references are unsupported. LINK_INPUT offsets
 * refer to the first path code unit in the original byte frame. This explicit
 * list does not establish transitive dependencies or linker command policy.
 * Output must be empty and is unchanged on every failure. */
XR_FUNC XrXirTargetStatus xtc_dependencies_parse(XrCompileResources *resources,
    const XrDependencyInput *input, XrDependencyLimits limits,
    XrDependencyFacts **output);
XR_FUNC XrCompileResources *xtc_dependencies_resources(const XrDependencyFacts *);
XR_FUNC XrXirTargetStatus xtc_dependencies_status(const XrDependencyFacts *);
XR_FUNC uint32_t xtc_dependencies_count(const XrDependencyFacts *);
XR_FUNC const XrDependencyRecord *xtc_dependencies_record(const XrDependencyFacts *, uint32_t);
/* NULL when the report does not contain a Make target. */
XR_FUNC const char *xtc_dependencies_target(const XrDependencyFacts *);
XR_FUNC XrXirTargetStatus xtc_dependencies_response_facts(const XrDependencyFacts *,
    XrDependencyResponseFacts *output);
XR_FUNC void xtc_dependencies_free(XrDependencyFacts *);
#endif // XTC_DEPENDENCIES_H
