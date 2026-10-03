/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstdlib_embedded.h - Embedded stdlib lookup API
 *
 * KEY CONCEPT:
 *   Exposes immutable source authority and separate runtime loading queries.
 *
 *   The descriptor table below is the whole answer to "which modules does this
 *   binary have, and what does loading one involve". It is generated from the
 *   declaration sources - the .xr sources this build selected, stdlib/defs and
 *   stdlib_boundary.toml - so no module needs a loader written for it.
 */

#ifndef XSTDLIB_EMBEDDED_H
#define XSTDLIB_EMBEDDED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../base/xdefs.h"
#include "../base/xio_policy.h"

struct XrModule;
struct XrVMRuntime;

typedef struct {
    const char *name;
    /* Canonical stdlib/<name>/<name>.xr text, or NULL for a module whose
     * semantics are still entirely native. A module with a source requires it:
     * loading fails rather than publishing an empty export table. */
    const char *source;
    bool has_native_entries;
} XrStdlibSourceDescriptor;

/* Look one module up in the generated table. NULL means this binary has no
 * such standard library module, which is what makes an import fall through to
 * the script and package resolvers. */
XR_FUNC const XrStdlibSourceDescriptor *xr_stdlib_source_descriptor(const char *module_name);

/* Compiler queries admit each table entry and compared byte before reading it.
 * Missing or ambiguous names publish NULL on OK; failures preserve output. */
XR_FUNC XrOsIoStatus xr_stdlib_source_descriptor_work(void *owner,
    XrOsIoStatus (*work)(void *, uint64_t), const char *name,
    const XrStdlibSourceDescriptor **output);
XR_FUNC XrOsIoStatus xr_get_embedded_stdlib_work(void *owner,
    XrOsIoStatus (*work)(void *, uint64_t), const char *name, const char **output);

// Get pre-compiled bytecode for a stdlib module.
// Returns NULL if module not found or no bytecode available.
XR_FUNC const uint8_t *xr_get_embedded_stdlib_bytecode(const char *module_name, size_t *out_size);

// Get source code for a stdlib module (fallback).
// Returns NULL if module not found.
XR_FUNC const char *xr_get_embedded_stdlib(const char *module_name);

// Install the generated native entries a module declares.
// A module that declares none succeeds without adding exports.
XR_FUNC bool xr_stdlib_module_install_native_entries(struct XrVMRuntime *isolate,
                                                     struct XrModule *module,
                                                     const char *requested_module_name);

#endif
