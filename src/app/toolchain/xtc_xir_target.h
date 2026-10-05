/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtc_xir_target.h - Owned observations of locked native compilation inputs
 *
 * KEY CONCEPT:
 *   File identity and lifetime do not prove that an observed input set is complete.
 */
#ifndef XTC_XIR_TARGET_H
#define XTC_XIR_TARGET_H
#include "base/xcompile_resources.h"

typedef enum XrXirTargetStatus {
    XR_XIR_TARGET_OK, XR_XIR_TARGET_INVALID, XR_XIR_TARGET_UNRESOLVED,
    XR_XIR_TARGET_BUDGET, XR_XIR_TARGET_OUT_OF_MEMORY, XR_XIR_TARGET_IO,
    XR_XIR_TARGET_UNSUPPORTED
} XrXirTargetStatus;
typedef enum XrXirTargetFileKind {
    XR_XIR_TARGET_COMPILER = 1, XR_XIR_TARGET_LINKER, XR_XIR_TARGET_PROVIDER_SUPPORT,
    XR_XIR_TARGET_HEADER, XR_XIR_TARGET_CRT_LIBRARY, XR_XIR_TARGET_SYSTEM_LIBRARY,
    XR_XIR_TARGET_SOURCE
} XrXirTargetFileKind;
typedef struct XrXirTargetDependency {
    const char *path;
    uint32_t kind;
} XrXirTargetDependency;
typedef struct XrXirTargetEnvironment {
    const char *key, *value;
} XrXirTargetEnvironment;
typedef struct XrXirTargetCommandFacts {
    const char *executable, *cwd;
    const char *const *argv;
    uint32_t argc;
    const XrXirTargetEnvironment *environment;
    uint32_t environment_count;
    uint32_t timeout_ms;
    uint64_t output_limit;
    /* Stable descriptive values: 0 = NONE, 1 = WINDOWS_TREE. No callback. */
    uint32_t image_mode;
    /* 0 waits for natural tree completion; 1 revokes execution after root EXIT. */
    uint32_t completion_policy;
} XrXirTargetCommandFacts;
struct XrXirImageCollector;
typedef struct XrXirTargetSnapshotRequest {
    XrCompileResources *resources;
    const char *triple;
    /* Descriptive neutral IDs: Clang=1, GCC=2, MSVC=3, Zig=4; MT=1, MD=2. */
    uint32_t provider, crt, dialect;
    const XrXirTargetDependency *files;
    uint32_t file_count;
    const XrXirTargetCommandFacts *commands;
    uint32_t command_count;
    const struct XrXirImageCollector *images;
} XrXirTargetSnapshotRequest;
typedef struct XrXirTargetFile {
    const char *path;
    uint32_t kind;
    uint64_t length;
    uint8_t digest[32];
} XrXirTargetFile;
typedef struct XrXirTargetFacts {
    uint32_t schema, provider, crt, dialect, file_count, command_count;
    const char *triple;
    uint8_t provider_identity[32], sysroot_identity[32], identity[32];
} XrXirTargetFacts;
typedef struct XrXirTargetSnapshot XrXirTargetSnapshot;
/* Output must be empty and remains unchanged on failure. No executable target
 * authority is issued. Commands describe explicit inputs, without inheriting
 * this process's environment. Executable and cwd must be absolute drive paths,
 * independently owned and valid UTF-8; no existence check is performed.
 * Timeout is nonzero and 0 < output_limit < SIZE_MAX. No evidence of execution
 * or prepared-process provenance is inferred from these descriptions. Keys are nonempty UTF-8; '=' is allowed only as
 * a leading system-reserved marker followed by a nonempty key. Windows keys
 * are unique under UTF-16 ordinal case-insensitive comparison. */
XR_FUNC XrXirTargetStatus xtc_xir_target_snapshot_capture(const XrXirTargetSnapshotRequest *request,
    XrXirTargetSnapshot **output);
XR_FUNC const XrXirTargetFacts *xtc_xir_target_facts(const XrXirTargetSnapshot *snapshot);
XR_FUNC const XrXirTargetFile *xtc_xir_target_file(const XrXirTargetSnapshot *snapshot, uint32_t index);
XR_FUNC const XrXirTargetCommandFacts *xtc_xir_target_command_facts(const XrXirTargetSnapshot *snapshot, uint32_t index);
XR_FUNC void xtc_xir_target_free(XrXirTargetSnapshot *snapshot);
#endif // XTC_XIR_TARGET_H
