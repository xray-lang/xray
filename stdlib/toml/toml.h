/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * toml.h - TOML standard library
 *
 * KEY CONCEPT:
 *   Provides TOML configuration file parsing and serialization.
 *   Compliant with TOML v1.0.0 specification.
 *
 * Functions:
 *   - parse(str)               Parse TOML string to Map
 *   - parseFile(path)          Parse TOML file
 *   - stringify(value)         Serialize Map to TOML string
 *   - stringify(value, indent) Serialize with indentation
 *   - writeFile(path, value)   Write TOML file
 *
 * Type mapping:
 *   TOML -> xray:
 *     String        -> string
 *     Integer       -> int
 *     Float         -> float
 *     Boolean       -> bool
 *     Datetime      -> string (ISO 8601 format)
 *     Array         -> Array
 *     Table         -> Map
 *     Inline Table  -> Map
 *
 *   xray -> TOML:
 *     string        -> String
 *     int           -> Integer
 *     float         -> Float
 *     bool          -> Boolean
 *     Array         -> Array
 *     Map           -> Table
 */

#ifndef XR_STDLIB_TOML_H
#define XR_STDLIB_TOML_H

#include "../../src/base/xdefs.h"
#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"

// Parse TOML string
// Returns: Map object
XR_FUNC XrValue xr_toml_parse(XrayIsolate *isolate, const char *data, size_t len);

// Serialize to TOML string
// value: Map object
// indent: requested number of indent spaces (0 = flat).
// NOTE: the `indent` parameter is currently reserved — the writer
// emits flat TOML regardless. Kept in the API for forward-compat; see
// the comment on TomlWriter.indent inside toml.c.
XR_FUNC XrValue xr_toml_stringify(XrayIsolate *isolate, XrValue value, int indent);

// Load toml module
XR_FUNC XrModule *xr_load_module_toml(XrayIsolate *isolate);

#endif
