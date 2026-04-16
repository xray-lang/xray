/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * csv.h - CSV standard library module
 *
 * KEY CONCEPT:
 *   High-performance CSV parser using state machine. Supports parsing,
 *   serialization, automatic type conversion, and flexible configuration.
 *
 * API:
 *   - parse(str [, config])         Parse CSV string
 *   - parseFile(path [, config])    Parse CSV file
 *   - parseDetailed(str, config)    Parse with detailed result (errors, meta)
 *   - stringify(data [, config])    Serialize to CSV string
 *   - writeFile(path, data)         Write CSV file
 *   - parseTsv(str)                 Parse TSV (tab-separated)
 *   - parseAuto(str)                Auto-detect delimiter
 *
 * Config options (Json object):
 *   - delimiter: string             Field separator, default ","
 *   - quoteChar: string             Quote character, default "\""
 *   - escapeChar: string            Escape character, default "\""
 *   - header: bool                  First row is header, default false
 *   - columns: Array                Custom column names
 *   - dynamicTyping: bool           Auto type conversion, default false
 *   - trimFields: bool              Trim field whitespace, default false
 *   - skipEmptyLines: bool          Skip empty lines, default true
 *   - comments: string              Comment prefix
 *   - skipRows: int                 Skip first N rows
 *   - maxRows: int                  Parse at most N rows
 *   - relaxQuotes: bool             Relaxed quote mode
 *   - relaxColumns: bool            Allow column count mismatch
 *
 * Type mapping:
 *   CSV row -> Array<string>
 *   CSV data -> Array<Array<string>>
 *   With header -> Array<Map<string, any>>
 *   dynamicTyping -> "123"->123, "true"->true
 */

#ifndef XR_STDLIB_CSV_H
#define XR_STDLIB_CSV_H

#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"

// Load csv module
XrModule* xr_load_module_csv(XrayIsolate *isolate);

#endif
