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
 * FILE FORMAT (.xrc):
 *   +----------------------------------------+
 *   | Header (16 bytes)                      |
 *   |   Magic: "XRAY" (4)                    |
 *   |   Version: u16                         |
 *   |   Flags: u16                           |
 *   |   Proto Count: u32                     |
 *   |   Reserved: u32                        |
 *   +----------------------------------------+
 *   | String Table                           |
 *   |   Count: u32                           |
 *   |   Strings: [len:u32, data:bytes]...    |
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

struct XrayIsolate;
struct XrProto;

#define XR_BC_MAGIC     0x59415258  // "XRAY" (little-endian)
#define XR_BC_VERSION   2           // v2: added shared_count

// Serialization flags
#define XR_BC_STRIP_DEBUG   (1 << 0)  // Remove debug info (line numbers, var names)
#define XR_BC_STRIP_SOURCE  (1 << 1)  // Remove source file path

typedef enum {
    XR_BC_OK = 0,
    XR_BC_ERR_MAGIC,
    XR_BC_ERR_VERSION,
    XR_BC_ERR_TRUNCATED,
    XR_BC_ERR_CORRUPT,
    XR_BC_ERR_ALLOC,
} XrBcError;

/* ========== Serialization API ========== */

// Serialize XrProto to byte array, caller must free
XR_FUNC uint8_t* xr_bytecode_write(struct XrayIsolate *X, struct XrProto *proto, 
                           int flags, size_t *out_size);

// Deserialize XrProto from byte array
XR_FUNC struct XrProto* xr_bytecode_read(struct XrayIsolate *X, const uint8_t *data, 
                                  size_t size, XrBcError *error);

// Execute bytecode directly, returns 0 on success
XR_FUNC int xr_eval_bytecode(struct XrayIsolate *X, const uint8_t *data, size_t size);

/* ========== File API ========== */

// Compile source file and save as bytecode
XR_FUNC bool xr_compile_to_file(struct XrayIsolate *X, const char *source_file, 
                        const char *output_file, int flags);

// Load and execute bytecode file
XR_FUNC int xr_run_bytecode_file(struct XrayIsolate *X, const char *bytecode_file);

/* ========== C Embedding Macros ========== */

#define XR_DECL_BYTECODE(name) \
    extern const uint8_t xr_bc_##name[]; \
    extern const uint32_t xr_bc_##name##_size

#define XR_EVAL_BYTECODE(X, name) \
    xr_eval_bytecode(X, xr_bc_##name, xr_bc_##name##_size)

/* ========== Output Format (for compile command) ========== */

typedef enum {
    XR_OUTPUT_AUTO,
    XR_OUTPUT_BYTECODE,
    XR_OUTPUT_C_SOURCE,
    XR_OUTPUT_C_HEADER,
} XrOutputFormat;

XR_FUNC XrOutputFormat xr_detect_output_format(const char *filename, XrOutputFormat explicit_fmt);

// Output as C source file
XR_FUNC bool xr_output_c_source(struct XrayIsolate *X, struct XrProto *proto,
                        const char *output_file, const char *var_name, int flags);

#endif // XBYTECODE_IO_H
