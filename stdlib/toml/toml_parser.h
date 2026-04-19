/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * toml_parser.h - TOML state machine parser
 *
 * KEY CONCEPT:
 *   High-performance TOML parser based on state machine.
 *
 * Features:
 *   - Single-pass O(n) complexity
 *   - SWAR fast number parsing
 *   - Strict mode support
 *   - Detailed error messages
 */

#ifndef XR_STDLIB_TOML_PARSER_H
#define XR_STDLIB_TOML_PARSER_H

#include "../../src/base/xdefs.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xjson.h"

// ========== Parser States ==========

typedef enum {
    TOML_STATE_LINE_START,    // Line start
    TOML_STATE_KEY,           // Parsing key
    TOML_STATE_KEY_DOT,       // Key dot separator
    TOML_STATE_EQUALS,        // After equals sign
    TOML_STATE_VALUE,         // Parsing value
    TOML_STATE_STRING,        // Inside basic string
    TOML_STATE_STRING_ML,     // Inside multiline string
    TOML_STATE_LITERAL,       // Inside literal string
    TOML_STATE_LITERAL_ML,    // Inside multiline literal string
    TOML_STATE_NUMBER,        // Number
    TOML_STATE_ARRAY,         // Inside array
    TOML_STATE_INLINE_TABLE,  // Inside inline table
    TOML_STATE_TABLE_HEADER,  // Table header [key]
    TOML_STATE_ARRAY_TABLE,   // Array table [[key]]
    TOML_STATE_COMMENT,       // Comment
    TOML_STATE_ERROR          // Error state
} TomlState;

// ========== Error Types ==========

typedef enum {
    TOML_ERROR_NONE,
    TOML_ERROR_SYNTAX,            // Syntax error
    TOML_ERROR_DUPLICATE_KEY,     // Duplicate key
    TOML_ERROR_INVALID_KEY,       // Invalid key
    TOML_ERROR_INVALID_VALUE,     // Invalid value
    TOML_ERROR_UNTERMINATED_STRING, // Unterminated string
    TOML_ERROR_INVALID_ESCAPE,    // Invalid escape
    TOML_ERROR_INVALID_NUMBER,    // Invalid number
    TOML_ERROR_INVALID_DATETIME,  // Invalid datetime
    TOML_ERROR_TABLE_CONFLICT     // Table definition conflict
} TomlErrorType;

// ========== Parser Configuration ==========

typedef struct {
    bool strict;                  // Strict mode
    bool allow_duplicate_keys;    // Allow duplicate keys
} TomlConfig;

// ========== Parse Metadata ==========

typedef struct {
    int lines;                    // Total lines
    int keys;                     // Key count
} TomlMeta;

// ========== Parse Result ==========

typedef struct {
    XrMap *data;                  // Parsed data
    XrArray *errors;              // Error list
    TomlMeta meta;                // Metadata
} TomlResult;

// ========== Parser Context ==========

typedef struct {
    XrayIsolate *isolate;

    // Input data
    const char *data;
    size_t len;
    size_t pos;

    // Position tracking
    int line;
    int column;

    // State machine
    TomlState state;

    // Current parsing context
    XrMap *root;                  // Root table
    XrMap *current_table;         // Current table
    XrArray *current_key_path;    // Current key path

    // Configuration
    TomlConfig config;

    // Result
    TomlResult result;

    // Temporary buffer
    char *temp_buf;
    size_t temp_len;
    size_t temp_cap;
} TomlParser;

// ========== API ==========

// Initialize parser configuration with default values
XR_FUNC void toml_config_init(TomlConfig *config);

// Extract configuration from Json object
XR_FUNC void toml_config_from_json(XrayIsolate *X, TomlConfig *config, XrJson *json);

// Initialize parser
XR_FUNC void toml_parser_init(TomlParser *parser, XrayIsolate *isolate,
                              const char *data, size_t len, TomlConfig *config);

// Execute parsing
XR_FUNC TomlResult toml_parser_parse(TomlParser *parser);

// Cleanup parser
XR_FUNC void toml_parser_cleanup(TomlParser *parser);

#endif
