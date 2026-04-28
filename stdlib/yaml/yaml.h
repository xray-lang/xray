/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * yaml.h - YAML parsing and serialization module
 *
 * KEY CONCEPT:
 *   Provides YAML parsing and serialization capabilities.
 *   Supports a common subset of YAML 1.2 core schema.
 *
 * API:
 *   - parse(str)               Parse YAML string
 *   - parseFile(path)          Parse YAML file
 *   - parseAll(str)            Parse multi-document YAML, returns array
 *   - stringify(value)         Serialize to YAML string
 *   - stringify(value, indent) Serialize with indentation
 *   - writeFile(path, value)   Write YAML file
 *
 * Type Mapping:
 *   YAML -> xray:
 *     !!null           -> null
 *     !!bool           -> bool
 *     !!int            -> int
 *     !!float          -> float
 *     !!str            -> string
 *     !!seq            -> Array
 *     !!map            -> Map
 *
 *   xray -> YAML:
 *     null             -> null/~
 *     bool             -> true/false
 *     int              -> integer
 *     float            -> floating point
 *     string           -> string (auto-select quotes)
 *     Array            -> sequence
 *     Map              -> mapping
 */

#ifndef XR_STDLIB_YAML_H
#define XR_STDLIB_YAML_H

#include "../../src/base/xdefs.h"
#include "../../src/runtime/value/xvalue.h"

struct XrModule;

// Parse YAML string (single document)
XR_FUNC XrValue xr_yaml_parse(XrayIsolate *isolate, const char *data, size_t len);

// Parse YAML string (multi-document)
// Returns: Array, each element is a document
XR_FUNC XrValue xr_yaml_parse_all(XrayIsolate *isolate, const char *data, size_t len);

// Serialize to YAML string
// indent: number of spaces for indentation, default 2.
// NOTE: the script-level `stringify(value, opts)` also accepts a
// `lineWidth` option; that parameter is currently reserved and does
// not yet trigger wrapping — see yaml_emitter.c for the full
// rationale.
XR_FUNC XrValue xr_yaml_stringify(XrayIsolate *isolate, XrValue value, int indent);

// Load yaml module
XR_FUNC struct XrModule *xr_load_module_yaml(XrayIsolate *isolate);

#endif  // XR_STDLIB_YAML_H
