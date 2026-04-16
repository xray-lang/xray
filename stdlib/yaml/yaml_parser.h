/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * yaml_parser.h - YAML state machine parser interface
 *
 * KEY CONCEPT:
 *   High-performance YAML parser with:
 *   - State machine architecture, single-pass
 *   - SWAR fast integer parsing
 *   - SIMD batch scanning
 *   - Anchor and alias support
 *   - Multi-document parsing
 */

#ifndef XR_STDLIB_YAML_PARSER_H
#define XR_STDLIB_YAML_PARSER_H

#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xjson.h"

// ========== Parse Configuration ==========

typedef struct {
    bool safe;                    // Safe mode (disable dangerous tags)
    bool allow_duplicate_keys;    // Allow duplicate keys
    int max_depth;                // Max nesting depth, default 64
} YamlConfig;

// ========== Parse Metadata ==========

typedef struct {
    int lines;                    // Total lines
    int documents;                // Document count
    int anchors;                  // Anchor count
} YamlMeta;

// ========== Parse Result ==========

typedef struct {
    XrValue data;                 // Parsed data
    XrArray *errors;              // Error list
    YamlMeta meta;                // Metadata
} YamlResult;

// ========== Anchor Storage ==========

#define YAML_MAX_ANCHORS 64

typedef struct {
    char name[64];
    XrValue value;
} YamlAnchor;

// ========== Parser Context ==========

typedef struct {
    XrayIsolate *isolate;
    
    // Input data
    const char *data;
    const char *ptr;
    const char *end;
    
    // Position tracking
    int line;
    int col;
    
    // Configuration
    YamlConfig config;
    
    // Anchor table
    YamlAnchor anchors[YAML_MAX_ANCHORS];
    int anchor_count;
    
    // Current nesting depth
    int depth;
    
    // Error message
    char error[256];
    
    // Result
    YamlResult result;
} YamlParser;

// ========== Common Value Conversions ==========

extern XrValue xr_string_value(XrString *str);
extern XrValue xr_value_from_array(XrArray *arr);
extern XrValue xr_value_from_map(XrMap *map);

// ========== API ==========

// Initialize parse config with defaults
void yaml_config_init(YamlConfig *config);

// Extract config from Json object
void yaml_config_from_json(XrayIsolate *X, YamlConfig *config, XrJson *json);

// Initialize parser
void yaml_parser_init(YamlParser *parser, XrayIsolate *isolate,
                      const char *data, size_t len, YamlConfig *config);

// Parse single document
XrValue yaml_parser_parse(YamlParser *parser);

// Parse all documents
XrArray* yaml_parser_parse_all(YamlParser *parser);

// Strict mode parsing
YamlResult yaml_parser_parse_strict(YamlParser *parser);

// Cleanup parser
void yaml_parser_cleanup(YamlParser *parser);

#endif
