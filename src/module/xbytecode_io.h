/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xbytecode_io.h - Bytecode serialization/deserialization
 *
 * KEY CONCEPT:
 *   Serializes XrProto to portable bytecode format and loads it back.
 *   Supports stripping debug info and embedding as C source.
 *
 * FILE FORMAT (.xrc), all integers little-endian:
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

#ifndef XBYTECODE_IO_H
#define XBYTECODE_IO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../base/xdefs.h"

struct XrVMRuntime;
struct XrCompilerSession;
struct XrProto;

// Serialization flags
#define XR_BC_STRIP_DEBUG (1 << 0)   // Remove debug info (line numbers, var names)
#define XR_BC_STRIP_SOURCE (1 << 1)  // Remove source file path

typedef enum {
    XR_BC_OK = 0,
    XR_BC_ERR_MAGIC,
    XR_BC_ERR_VERSION,
    XR_BC_ERR_TRUNCATED,
    XR_BC_ERR_CORRUPT,
    XR_BC_ERR_ALLOC,
    XR_BC_ERR_METADATA,
    XR_BC_ERR_TARGET_ABI,
} XrBcError;

XR_FUNC const char *xr_bytecode_error_string(XrBcError error);

/* ========== Serialization API ========== */

// Serialize XrProto to byte array, caller must free
XR_FUNC uint8_t *xr_bytecode_write(struct XrVMRuntime *X, struct XrProto *proto, int flags,
                                   size_t *out_size, XrBcError *error);

/* Serialize a built-in stdlib module.  The canonical module identity is part
 * of the compilation unit, not inferred from load order: enum constants that
 * also have native producers are encoded as that module's nominal type. */
XR_FUNC uint8_t *xr_bytecode_write_stdlib(struct XrVMRuntime *X, const char *canonical_module,
                                          struct XrProto *proto, int flags, size_t *out_size,
                                          XrBcError *error);

// Deserialize XrProto from byte array
XR_FUNC struct XrProto *xr_bytecode_read(struct XrVMRuntime *X, const uint8_t *data, size_t size,
                                         XrBcError *error);

// Execute bytecode directly, returns 0 on success
XR_FUNC int xr_eval_bytecode(struct XrVMRuntime *X, const uint8_t *data, size_t size);

/* ========== File API ========== */

// Compile source file with an explicit compiler session and save as bytecode.
XR_FUNC bool xr_compile_to_file(struct XrCompilerSession *session, const char *source_file,
                                const char *output_file, int flags);
XR_FUNC bool xr_compile_stdlib_to_file(struct XrCompilerSession *session,
                                       const char *canonical_module, const char *source_file,
                                       const char *output_file, int flags);

// Load and execute bytecode file
XR_FUNC int xr_run_bytecode_file(struct XrVMRuntime *X, const char *bytecode_file);

/* ========== C Embedding Macros ========== */

#define XR_DECL_BYTECODE(name)                                                                     \
    extern const uint8_t xr_bc_##name[];                                                           \
    extern const uint32_t xr_bc_##name##_size

#define XR_EVAL_BYTECODE(X, name) xr_eval_bytecode(X, xr_bc_##name, xr_bc_##name##_size)

/* ========== Output Format (for compile command) ========== */

typedef enum {
    XR_OUTPUT_AUTO,
    XR_OUTPUT_BYTECODE,
    XR_OUTPUT_C_SOURCE,
    XR_OUTPUT_C_HEADER,
} XrOutputFormat;

XR_FUNC XrOutputFormat xr_detect_output_format(const char *filename, XrOutputFormat explicit_fmt);

// Output as C source file
XR_FUNC bool xr_output_c_source(struct XrVMRuntime *X, struct XrProto *proto,
                                const char *output_file, const char *var_name, int flags);

#endif  // XBYTECODE_IO_H
