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
 *   Provides access to pre-compiled stdlib modules embedded as C arrays.
 *   Two lookup modes: bytecode (preferred) and source fallback.
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

struct XrModule;
struct XrVMRuntime;

/* Installs a module's declared native entries. Generated per module by
 * tools/stdlibgen/stdlibgen.py; answers false when the installed export count
 * is not the declared one, so a partial install fails the load. */
typedef bool (*XrStdlibNativeEntryBinder)(struct XrVMRuntime *isolate, struct XrModule *module);

typedef struct {
    const char *name;
    /* Canonical stdlib/<name>/<name>.xr text, or NULL for a module whose
     * semantics are still entirely native. A module with a source requires it:
     * loading fails rather than publishing an empty export table. */
    const char *source;
    XrStdlibNativeEntryBinder bind_native_entries;
} XrStdlibModuleDescriptor;

/* Look one module up in the generated table. NULL means this binary has no
 * such standard library module, which is what makes an import fall through to
 * the script and package resolvers. */
XR_FUNC const XrStdlibModuleDescriptor *xr_stdlib_module_descriptor(const char *module_name);

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
