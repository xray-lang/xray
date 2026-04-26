/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * csv_parser.h - CSV state machine parser
 *
 * KEY CONCEPT:
 *   High-performance CSV parser using state machine. Features:
 *   - Single-pass O(n) complexity
 *   - Zero-copy field extraction
 *   - Automatic type conversion
 *   - Detailed error reporting
 */

#ifndef XR_STDLIB_CSV_PARSER_H
#define XR_STDLIB_CSV_PARSER_H

#include "../../src/base/xdefs.h"
#include "../../src/runtime/xisolate_internal.h"
#include "../../src/runtime/object/xarray.h"
#include "../../src/runtime/object/xmap.h"
#include "../../src/runtime/object/xjson.h"

/* ========== Parser States ========== */

typedef enum {
    CSV_STATE_FIELD_START,  // Field start
    CSV_STATE_UNQUOTED,     // Unquoted field
    CSV_STATE_QUOTED,       // Inside quoted field
    CSV_STATE_QUOTE_END,    // Quote end or escape
    CSV_STATE_ROW_END,      // Row end
    CSV_STATE_ERROR         // Error state
} CsvParseState;

/* ========== Error Types ========== */

typedef enum {
    CSV_ERROR_NONE,
    CSV_ERROR_UNTERMINATED_QUOTE,  // Unterminated quote
    CSV_ERROR_FIELD_MISMATCH,      // Column count mismatch
    CSV_ERROR_INVALID_ESCAPE,      // Invalid escape
    CSV_ERROR_INVALID_ROW          // Invalid row
} CsvErrorType;

/* ========== Error Info ========== */

typedef struct {
    CsvErrorType type;
    int row;
    int column;
    char message[128];
} CsvError;

/* ========== Parse Config ========== */

typedef struct {
    char delimiter;    // Field separator, default ','
    char quote_char;   // Quote character, default '"'
    char escape_char;  // Escape character, default '"'

    bool header;       // First row is header
    XrArray *columns;  // Custom column names

    bool dynamic_typing;    // Auto type conversion
    bool trim_fields;       // Trim field whitespace
    bool skip_empty_lines;  // Skip empty lines

    // When dynamic_typing is true, any field whose string form matches
    // one of these values is returned as null instead of the default
    // (empty string -> null only). The default list is ["", "null",
    // "NULL"] so a literal `null` cell becomes xr_null(); callers that
    // need to preserve literal "null" text should pass `nullStrings:
    // []` to disable matching entirely. Matching is case-sensitive
    // (pass both casings if needed).
    XrArray *null_strings;  // GC-rooted; borrowed, not owned

    // Owned NUL-terminated comment prefix. Previously this was a bare
    // `const char *` copied from the caller's XrString which left a
    // dangling pointer if the Json argument (or the interned string it
    // contained) was reclaimed before parse finished. The fixed-size
    // buffer keeps the config self-contained at negligible cost.
    char comments[16];
    bool has_comments;
    int skip_rows;  // Skip first N rows
    int max_rows;   // Parse at most N rows, 0 = unlimited

    bool relax_quotes;   // Relaxed quote mode
    bool relax_columns;  // Allow column count mismatch

    // Line terminator used by stringify(). Not consulted by the parser
    // (which transparently accepts any CR/LF mix). Defaults to "\n";
    // callers that need RFC 4180 compliance can opt in with "\r\n".
    char linebreak[3];
} CsvConfig;

/* ========== Parse Metadata ========== */

typedef struct {
    int rows;           // Total rows
    int columns;        // Column count
    char delimiter;     // Detected delimiter
    char linebreak[3];  // Detected line break
    bool truncated;     // Truncated due to max_rows
    bool aborted;       // Aborted due to error
} CsvMeta;

/* ========== Parse Result ========== */

typedef struct {
    XrArray *data;    // Parsed data
    XrArray *errors;  // Error list (each element is a Map)
    CsvMeta meta;     // Metadata
} CsvResult;

/* ========== Parser Context ========== */

typedef struct {
    XrayIsolate *isolate;

    // Input data (no copy)
    const char *data;
    size_t len;
    size_t pos;

    // State machine
    CsvParseState state;

    // Current field
    size_t field_start;
    bool field_quoted;

    // Current row
    XrArray *current_row;
    int current_row_num;
    int current_col_num;
    int expected_columns;

    // Config
    CsvConfig config;

    // Result
    CsvResult result;

    // Temp buffer (used for escape handling)
    char *temp_buf;
    size_t temp_len;
    size_t temp_cap;
} CsvParser;

/* ========== Value Constructors (shared by csv.c and csv_parser.c) ========== */

extern XrValue xr_string_value(XrString *str);
extern XrValue xr_value_from_array(XrArray *arr);
extern XrValue xr_value_from_map(XrMap *map);

/* ========== API ========== */

// Initialize parser config to defaults
XR_FUNC void csv_config_init(CsvConfig *config);

// Extract config from Json object
XR_FUNC void csv_config_from_json(XrayIsolate *X, CsvConfig *config, XrJson *json);

// Initialize parser
XR_FUNC void csv_parser_init(CsvParser *parser, XrayIsolate *isolate, const char *data, size_t len,
                             CsvConfig *config);

// Execute parsing
XR_FUNC void csv_parser_parse(CsvParser *parser);

// Cleanup parser
XR_FUNC void csv_parser_cleanup(CsvParser *parser);

// Auto-detect delimiter
XR_FUNC char csv_detect_delimiter(const char *data, size_t len);

// Auto type conversion
XR_FUNC XrValue csv_convert_value(XrayIsolate *isolate, const char *field, size_t len);

#endif
