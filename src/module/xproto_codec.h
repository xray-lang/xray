/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xproto_codec.h - Compiler-internal proto container codec
 *
 * KEY CONCEPT:
 *   Serializes XrProto to portable bytecode format and loads it back.
 *   Supports stripping debug info and embedding as C source.
 *
 * INTERNAL PROTO CONTAINER, all integers little-endian:
 *   +----------------------------------------+
 *   | Header (20 bytes)                      |
 *   |   Magic: "XRAY" (4)                    |
 *   |   Version: u16                         |
 *   |   Flags: u16                           |
 *   |   Proto Count: u32                     |
 *   |   Max Symbol ID: u32                   |
 *   |   Shared Count: u32                    |
 *   +----------------------------------------+
 *   | Canonical Aggregate Layout Table       |
 *   |   Count: u32                           |
 *   |   Records sorted by stable semantic ID |
 *   +----------------------------------------+
 *   | Symbol Table                           |
 *   +----------------------------------------+
 *   | Proto Section                          |
 *   |   [Proto]*                             |
 *   +----------------------------------------+
 */

#ifndef XPROTO_CODEC_H
#define XPROTO_CODEC_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../base/xdefs.h"

struct XrVMRuntime;
struct XrCompilerSession;
struct XrProto;

/* Internal compiler/bootstrap container identity.  This is not an installed
 * artifact kind and is never accepted by the public runtime loader. */
#define XR_BOOTSTRAP_CONTAINER_MAGIC_SIZE 4u
#define XR_BOOTSTRAP_CONTAINER_VERSION UINT16_C(30)

/* Serialization flags for compiler/bootstrap-only containers. */
#define XR_BOOTSTRAP_CONTAINER_STRIP_DEBUG (1 << 0)
#define XR_BOOTSTRAP_CONTAINER_STRIP_SOURCE (1 << 1)

typedef enum {
    XR_BOOTSTRAP_CONTAINER_OK = 0,
    XR_BOOTSTRAP_CONTAINER_ERR_MAGIC,
    XR_BOOTSTRAP_CONTAINER_ERR_VERSION,
    XR_BOOTSTRAP_CONTAINER_ERR_TRUNCATED,
    XR_BOOTSTRAP_CONTAINER_ERR_CORRUPT,
    XR_BOOTSTRAP_CONTAINER_ERR_ALLOC,
    XR_BOOTSTRAP_CONTAINER_ERR_METADATA,
    XR_BOOTSTRAP_CONTAINER_ERR_TARGET_ABI,
} XrBootstrapContainerError;

XR_FUNC const char *xr_bootstrap_container_error_string(XrBootstrapContainerError error);

/* ========== Serialization API ========== */

/* Serialize XrProto to a byte array owned by the caller. */
XR_FUNC uint8_t *xr_bootstrap_container_write(
    struct XrVMRuntime *X, struct XrProto *proto, int flags, size_t *out_size,
    XrBootstrapContainerError *error);

/* Serialize a built-in stdlib module.  The canonical module identity is part
 * of the compilation unit, not inferred from load order: enum constants that
 * also have native producers are encoded as that module's nominal type. */
XR_FUNC uint8_t *xr_bootstrap_container_write_stdlib(
    struct XrVMRuntime *X, const char *canonical_module, struct XrProto *proto, int flags,
    size_t *out_size, XrBootstrapContainerError *error);

/* Deserialize XrProto from a bootstrap container. */
XR_FUNC struct XrProto *xr_bootstrap_container_read(
    struct XrVMRuntime *X, const uint8_t *data, size_t size, XrBootstrapContainerError *error);

/* Execute an internal bootstrap container, returning zero on success. */
XR_FUNC int xr_bootstrap_container_execute(
    struct XrVMRuntime *X, const uint8_t *data, size_t size);

/* ========== Internal bootstrap file API ========== */

// Compile source file with an explicit compiler session and save an internal
// bootstrap container. These functions are not installed product routes.
XR_FUNC bool xr_compile_to_file(struct XrCompilerSession *session, const char *source_file,
                                const char *output_file, int flags);
XR_FUNC bool xr_compile_stdlib_to_file(struct XrCompilerSession *session,
                                       const char *canonical_module, const char *source_file,
                                       const char *output_file, int flags);

/* Emit an internal bootstrap container as a C source array. */
XR_FUNC bool xr_bootstrap_container_emit_c_source(
    struct XrVMRuntime *X, struct XrProto *proto, const char *output_file,
    const char *var_name, int flags);

#endif  // XPROTO_CODEC_H
