/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_stdlib.c - Built-in standard library symbols
 */

#include "xlsp_stdlib.h"
#include <string.h>

// ============================================================================
// time module
// ============================================================================

static const XlspParamInfo time_sleep_params[] = {{"ms", "int", "Milliseconds to sleep"}};

static const XlspSymbolInfo time_symbols[] = {
    {"now", XLSP_SYM_FUNCTION, "fn(): int", "Returns current Unix timestamp in milliseconds", NULL,
     0},
    {"sleep", XLSP_SYM_FUNCTION, "fn(ms: int)", "Suspends execution for the specified milliseconds",
     time_sleep_params, 1},
    {"monotonic", XLSP_SYM_FUNCTION, "fn(): int", "Returns monotonic clock time in milliseconds",
     NULL, 0},
    {"clock", XLSP_SYM_FUNCTION, "fn(): float", "Returns high-resolution CPU time in seconds", NULL,
     0},
};

// ============================================================================
// json module
// ============================================================================

static const XlspParamInfo json_parse_params[] = {{"text", "string", "JSON string to parse"}};

static const XlspParamInfo json_stringify_params[] = {
    {"value", "Json", "Value to convert to JSON"},
    {"indent", "int", "Indentation spaces (optional)"}};

static const XlspSymbolInfo json_symbols[] = {
    {"parse", XLSP_SYM_FUNCTION, "fn(text: string): Json",
     "Parses a JSON string and returns the corresponding value", json_parse_params, 1},
    {"stringify", XLSP_SYM_FUNCTION, "fn(value: Json, indent?: int): string",
     "Converts a value to a JSON string", json_stringify_params, 2},
};

// ============================================================================
// http module
// ============================================================================

static const XlspParamInfo http_route_params[] = {
    {"method", "string", "HTTP method (GET, POST, etc.)"},
    {"path", "string", "URL path pattern"},
    {"handler", "fn(req): Json", "Request handler function"}};

static const XlspParamInfo http_listen_params[] = {{"port", "int", "Port number to listen on"}};

static const XlspParamInfo http_get_params[] = {{"url", "string", "URL to fetch"}};

static const XlspParamInfo http_post_params[] = {
    {"url", "string", "URL to post to"},
    {"body", "string", "Request body"},
    {"headers", "Map<string,string>", "Optional headers"}};

static const XlspSymbolInfo http_symbols[] = {
    {"route", XLSP_SYM_FUNCTION, "fn(method: string, path: string, handler: fn)",
     "Registers a route handler", http_route_params, 3},
    {"listen", XLSP_SYM_FUNCTION, "fn(port: int)", "Starts the HTTP server on the specified port",
     http_listen_params, 1},
    {"get", XLSP_SYM_FUNCTION, "fn(url: string): Response", "Performs an HTTP GET request",
     http_get_params, 1},
    {"post", XLSP_SYM_FUNCTION, "fn(url: string, body: string, headers?: Map): Response",
     "Performs an HTTP POST request", http_post_params, 3},
};

// ============================================================================
// regex module (via /pattern/ syntax)
// ============================================================================

static const XlspParamInfo regex_test_params[] = {{"text", "string", "String to test against"}};

static const XlspParamInfo regex_find_params[] = {{"text", "string", "String to search in"}};

static const XlspParamInfo regex_replace_params[] = {
    {"text", "string", "String to modify"}, {"replacement", "string", "Replacement string"}};

static const XlspParamInfo regex_split_params[] = {{"text", "string", "String to split"}};

static const XlspSymbolInfo regex_symbols[] = {
    {"test", XLSP_SYM_METHOD, "fn(text: string): bool", "Tests if the pattern matches the string",
     regex_test_params, 1},
    {"find", XLSP_SYM_METHOD, "fn(text: string): string?", "Finds the first match in the string",
     regex_find_params, 1},
    {"findAll", XLSP_SYM_METHOD, "fn(text: string): Array<string>",
     "Finds all matches in the string", regex_find_params, 1},
    {"replace", XLSP_SYM_METHOD, "fn(text: string, replacement: string): string",
     "Replaces matches with replacement string", regex_replace_params, 2},
    {"split", XLSP_SYM_METHOD, "fn(text: string): Array<string>",
     "Splits the string by the pattern", regex_split_params, 1},
};

// ============================================================================
// log module
// ============================================================================

static const XlspParamInfo log_msg_params[] = {{"message", "Json", "Message to log"},
                                               {"...", "Json", "Additional values"}};

static const XlspParamInfo log_setLevel_params[] = {
    {"level", "int", "Log level (DEBUG=0, INFO=1, WARN=2, ERROR=3, FATAL=4)"}};

static const XlspParamInfo log_setFormat_params[] = {
    {"format", "string", "Format: 'text' or 'json'"}};

static const XlspSymbolInfo log_symbols[] = {
    {"debug", XLSP_SYM_FUNCTION, "fn(message: Json, ...)", "Logs a debug message", log_msg_params,
     2},
    {"info", XLSP_SYM_FUNCTION, "fn(message: Json, ...)", "Logs an info message", log_msg_params,
     2},
    {"warn", XLSP_SYM_FUNCTION, "fn(message: Json, ...)", "Logs a warning message", log_msg_params,
     2},
    {"error", XLSP_SYM_FUNCTION, "fn(message: Json, ...)", "Logs an error message", log_msg_params,
     2},
    {"fatal", XLSP_SYM_FUNCTION, "fn(message: Json, ...)", "Logs a fatal message and exits",
     log_msg_params, 2},
    {"setLevel", XLSP_SYM_FUNCTION, "fn(level: int)", "Sets the minimum log level",
     log_setLevel_params, 1},
    {"setFormat", XLSP_SYM_FUNCTION, "fn(format: string)", "Sets log output format",
     log_setFormat_params, 1},
    {"DEBUG", XLSP_SYM_CONSTANT, "int", "Debug level (0)", NULL, 0},
    {"INFO", XLSP_SYM_CONSTANT, "int", "Info level (1)", NULL, 0},
    {"WARN", XLSP_SYM_CONSTANT, "int", "Warning level (2)", NULL, 0},
    {"ERROR", XLSP_SYM_CONSTANT, "int", "Error level (3)", NULL, 0},
    {"FATAL", XLSP_SYM_CONSTANT, "int", "Fatal level (4)", NULL, 0},
};

// ============================================================================
// math module
// ============================================================================

static const XlspParamInfo math_unary_params[] = {{"x", "float", "Input value"}};

static const XlspParamInfo math_binary_params[] = {{"x", "float", "First value"},
                                                   {"y", "float", "Second value"}};

static const XlspParamInfo math_random_params[] = {{"min", "float", "Minimum value (optional)"},
                                                   {"max", "float", "Maximum value (optional)"}};

static const XlspSymbolInfo math_symbols[] = {
    {"abs", XLSP_SYM_FUNCTION, "fn(x: float): float", "Returns absolute value", math_unary_params,
     1},
    {"floor", XLSP_SYM_FUNCTION, "fn(x: float): int", "Rounds down to nearest integer",
     math_unary_params, 1},
    {"ceil", XLSP_SYM_FUNCTION, "fn(x: float): int", "Rounds up to nearest integer",
     math_unary_params, 1},
    {"round", XLSP_SYM_FUNCTION, "fn(x: float): int", "Rounds to nearest integer",
     math_unary_params, 1},
    {"sqrt", XLSP_SYM_FUNCTION, "fn(x: float): float", "Returns square root", math_unary_params, 1},
    {"pow", XLSP_SYM_FUNCTION, "fn(x: float, y: float): float", "Returns x raised to power y",
     math_binary_params, 2},
    {"min", XLSP_SYM_FUNCTION, "fn(x: float, y: float): float", "Returns smaller value",
     math_binary_params, 2},
    {"max", XLSP_SYM_FUNCTION, "fn(x: float, y: float): float", "Returns larger value",
     math_binary_params, 2},
    {"sin", XLSP_SYM_FUNCTION, "fn(x: float): float", "Returns sine", math_unary_params, 1},
    {"cos", XLSP_SYM_FUNCTION, "fn(x: float): float", "Returns cosine", math_unary_params, 1},
    {"tan", XLSP_SYM_FUNCTION, "fn(x: float): float", "Returns tangent", math_unary_params, 1},
    {"random", XLSP_SYM_FUNCTION, "fn(min?: float, max?: float): float", "Returns random number",
     math_random_params, 2},
    {"PI", XLSP_SYM_CONSTANT, "float", "Pi (3.14159...)", NULL, 0},
    {"E", XLSP_SYM_CONSTANT, "float", "Euler's number (2.71828...)", NULL, 0},
};

// ============================================================================
// os module
// ============================================================================

static const XlspParamInfo os_getenv_params[] = {{"name", "string", "Environment variable name"}};

static const XlspParamInfo os_setenv_params[] = {{"name", "string", "Environment variable name"},
                                                 {"value", "string", "Value to set"}};

static const XlspParamInfo os_exec_params[] = {{"command", "string", "Command to execute"}};

static const XlspSymbolInfo os_symbols[] = {
    {"getenv", XLSP_SYM_FUNCTION, "fn(name: string): string?", "Gets environment variable value",
     os_getenv_params, 1},
    {"setenv", XLSP_SYM_FUNCTION, "fn(name: string, value: string)", "Sets environment variable",
     os_setenv_params, 2},
    {"exec", XLSP_SYM_FUNCTION, "fn(command: string): int",
     "Executes shell command, returns exit code", os_exec_params, 1},
    {"exit", XLSP_SYM_FUNCTION, "fn(code: int)", "Exits the process with code", NULL, 0},
    {"args", XLSP_SYM_VARIABLE, "Array<string>", "Command line arguments", NULL, 0},
    {"platform", XLSP_SYM_CONSTANT, "string", "Operating system name", NULL, 0},
};

// ============================================================================
// fs module
// ============================================================================

static const XlspParamInfo fs_read_params[] = {{"path", "string", "File path to read"}};

static const XlspParamInfo fs_write_params[] = {{"path", "string", "File path to write"},
                                                {"content", "string", "Content to write"}};

static const XlspParamInfo fs_path_params[] = {{"path", "string", "File or directory path"}};

static const XlspSymbolInfo fs_symbols[] = {
    {"readFile", XLSP_SYM_FUNCTION, "fn(path: string): string", "Reads entire file as string",
     fs_read_params, 1},
    {"writeFile", XLSP_SYM_FUNCTION, "fn(path: string, content: string)", "Writes string to file",
     fs_write_params, 2},
    {"appendFile", XLSP_SYM_FUNCTION, "fn(path: string, content: string)", "Appends string to file",
     fs_write_params, 2},
    {"exists", XLSP_SYM_FUNCTION, "fn(path: string): bool", "Checks if path exists", fs_path_params,
     1},
    {"isFile", XLSP_SYM_FUNCTION, "fn(path: string): bool", "Checks if path is a file",
     fs_path_params, 1},
    {"isDir", XLSP_SYM_FUNCTION, "fn(path: string): bool", "Checks if path is a directory",
     fs_path_params, 1},
    {"readDir", XLSP_SYM_FUNCTION, "fn(path: string): Array<string>", "Lists directory contents",
     fs_path_params, 1},
    {"mkdir", XLSP_SYM_FUNCTION, "fn(path: string)", "Creates directory", fs_path_params, 1},
    {"remove", XLSP_SYM_FUNCTION, "fn(path: string)", "Removes file or empty directory",
     fs_path_params, 1},
};

// ============================================================================
// crypto module
// ============================================================================

static const XlspParamInfo crypto_hash_params[] = {{"data", "string", "Data to hash"}};

static const XlspParamInfo crypto_random_params[] = {{"length", "int", "Number of random bytes"}};

static const XlspSymbolInfo crypto_symbols[] = {
    {"md5", XLSP_SYM_FUNCTION, "fn(data: string): string", "Computes MD5 hash (hex)",
     crypto_hash_params, 1},
    {"sha1", XLSP_SYM_FUNCTION, "fn(data: string): string", "Computes SHA-1 hash (hex)",
     crypto_hash_params, 1},
    {"sha256", XLSP_SYM_FUNCTION, "fn(data: string): string", "Computes SHA-256 hash (hex)",
     crypto_hash_params, 1},
    {"randomBytes", XLSP_SYM_FUNCTION, "fn(length: int): Bytes",
     "Generates cryptographically secure random bytes", crypto_random_params, 1},
};

// ============================================================================
// base64 module
// ============================================================================

static const XlspParamInfo base64_encode_params[] = {{"data", "string", "Data to encode"}};

static const XlspParamInfo base64_decode_params[] = {
    {"encoded", "string", "Base64 string to decode"}};

static const XlspSymbolInfo base64_symbols[] = {
    {"encode", XLSP_SYM_FUNCTION, "fn(data: string): string", "Encodes string to Base64",
     base64_encode_params, 1},
    {"decode", XLSP_SYM_FUNCTION, "fn(encoded: string): string", "Decodes Base64 string",
     base64_decode_params, 1},
};

// ============================================================================
// datetime module
// ============================================================================

static const XlspParamInfo datetime_create_params[] = {
    {"year", "int", "Year"},
    {"month", "int", "Month (1-12)"},
    {"day", "int", "Day (1-31)"},
    {"hour", "int", "Hour (0-23, optional)"},
    {"minute", "int", "Minute (0-59, optional)"},
    {"second", "int", "Second (0-59, optional)"}};

static const XlspParamInfo datetime_parse_params[] = {
    {"text", "string", "Date/time string to parse"},
    {"format", "string", "Format pattern (optional)"}};

static const XlspParamInfo datetime_timestamp_params[] = {
    {"ms", "int", "Unix timestamp in milliseconds"}};

static const XlspParamInfo datetime_format_params[] = {
    {"pattern", "string", "Format pattern (e.g. 'YYYY-MM-DD')"}};

static const XlspParamInfo datetime_add_params[] = {
    {"amount", "int", "Amount to add"},
    {"unit", "string", "Unit: 'year', 'month', 'day', 'hour', 'minute', 'second'"}};

static const XlspParamInfo datetime_diff_params[] = {
    {"other", "DateTime", "Another DateTime to compare"}, {"unit", "string", "Unit for result"}};

static const XlspSymbolInfo datetime_symbols[] = {
    {"now", XLSP_SYM_FUNCTION, "fn(): DateTime", "Returns current local date/time", NULL, 0},
    {"utc", XLSP_SYM_FUNCTION, "fn(): DateTime", "Returns current UTC date/time", NULL, 0},
    {"create", XLSP_SYM_FUNCTION,
     "fn(year: int, month: int, day: int, hour?: int, minute?: int, second?: int): DateTime",
     "Creates a DateTime from components", datetime_create_params, 6},
    {"fromTimestamp", XLSP_SYM_FUNCTION, "fn(ms: int): DateTime",
     "Creates a DateTime from Unix timestamp (milliseconds)", datetime_timestamp_params, 1},
    {"parse", XLSP_SYM_FUNCTION, "fn(text: string, format?: string): DateTime",
     "Parses a date/time string", datetime_parse_params, 2},
    {"offset", XLSP_SYM_FUNCTION, "fn(): int", "Returns local timezone offset in minutes", NULL, 0},
    {"format", XLSP_SYM_FUNCTION, "fn(dt: DateTime, pattern: string): string",
     "Formats a DateTime to string", datetime_format_params, 1},
    {"year", XLSP_SYM_FUNCTION, "fn(dt: DateTime): int", "Gets the year component", NULL, 0},
    {"month", XLSP_SYM_FUNCTION, "fn(dt: DateTime): int", "Gets the month component (1-12)", NULL,
     0},
    {"day", XLSP_SYM_FUNCTION, "fn(dt: DateTime): int", "Gets the day component (1-31)", NULL, 0},
    {"hour", XLSP_SYM_FUNCTION, "fn(dt: DateTime): int", "Gets the hour component (0-23)", NULL, 0},
    {"minute", XLSP_SYM_FUNCTION, "fn(dt: DateTime): int", "Gets the minute component (0-59)", NULL,
     0},
    {"second", XLSP_SYM_FUNCTION, "fn(dt: DateTime): int", "Gets the second component (0-59)", NULL,
     0},
    {"weekday", XLSP_SYM_FUNCTION, "fn(dt: DateTime): int",
     "Gets the weekday (0=Sunday, 6=Saturday)", NULL, 0},
    {"timestamp", XLSP_SYM_FUNCTION, "fn(dt: DateTime): int", "Gets Unix timestamp in milliseconds",
     NULL, 0},
    {"add", XLSP_SYM_FUNCTION, "fn(dt: DateTime, amount: int, unit: string): DateTime",
     "Adds time to a DateTime", datetime_add_params, 2},
    {"diff", XLSP_SYM_FUNCTION, "fn(dt1: DateTime, dt2: DateTime, unit: string): int",
     "Calculates difference between two DateTimes", datetime_diff_params, 2},
    {"toUTC", XLSP_SYM_FUNCTION, "fn(dt: DateTime): DateTime", "Converts to UTC timezone", NULL, 0},
    {"toLocal", XLSP_SYM_FUNCTION, "fn(dt: DateTime): DateTime", "Converts to local timezone", NULL,
     0},
};

// ============================================================================
// path module
// ============================================================================

static const XlspSymbolInfo path_symbols[] = {
    {"join", XLSP_SYM_FUNCTION, "fn(...paths: string): string", "Joins path segments", NULL, 0},
    {"dirname", XLSP_SYM_FUNCTION, "fn(path: string): string", "Returns directory name", NULL, 0},
    {"basename", XLSP_SYM_FUNCTION, "fn(path: string): string", "Returns base name", NULL, 0},
    {"extname", XLSP_SYM_FUNCTION, "fn(path: string): string", "Returns file extension", NULL, 0},
    {"normalize", XLSP_SYM_FUNCTION, "fn(path: string): string", "Normalizes path", NULL, 0},
    {"isAbsolute", XLSP_SYM_FUNCTION, "fn(path: string): bool", "Checks if path is absolute", NULL,
     0},
    {"resolve", XLSP_SYM_FUNCTION, "fn(...paths: string): string", "Resolves to absolute path",
     NULL, 0},
    {"relative", XLSP_SYM_FUNCTION, "fn(from: string, to: string): string", "Returns relative path",
     NULL, 0},
    {"parse", XLSP_SYM_FUNCTION, "fn(path: string): Json", "Parses path into components", NULL, 0},
    {"format", XLSP_SYM_FUNCTION, "fn(obj: Json): string", "Formats path object to string", NULL,
     0},
    {"sep", XLSP_SYM_CONSTANT, "string", "Path separator", NULL, 0},
    {"delimiter", XLSP_SYM_CONSTANT, "string", "Path delimiter", NULL, 0},
};

// ============================================================================
// string module
// ============================================================================

static const XlspSymbolInfo string_symbols[] = {
    {"length", XLSP_SYM_FUNCTION, "fn(s: string): int", "Returns string length", NULL, 0},
    {"upper", XLSP_SYM_FUNCTION, "fn(s: string): string", "Converts to uppercase", NULL, 0},
    {"lower", XLSP_SYM_FUNCTION, "fn(s: string): string", "Converts to lowercase", NULL, 0},
    {"trim", XLSP_SYM_FUNCTION, "fn(s: string): string", "Trims whitespace from both ends", NULL,
     0},
    {"trimLeft", XLSP_SYM_FUNCTION, "fn(s: string): string", "Trims whitespace from start", NULL,
     0},
    {"trimRight", XLSP_SYM_FUNCTION, "fn(s: string): string", "Trims whitespace from end", NULL, 0},
    {"startsWith", XLSP_SYM_FUNCTION, "fn(s: string, prefix: string): bool",
     "Checks if starts with prefix", NULL, 0},
    {"endsWith", XLSP_SYM_FUNCTION, "fn(s: string, suffix: string): bool",
     "Checks if ends with suffix", NULL, 0},
    {"contains", XLSP_SYM_FUNCTION, "fn(s: string, sub: string): bool",
     "Checks if contains substring", NULL, 0},
    {"indexOf", XLSP_SYM_FUNCTION, "fn(s: string, sub: string): int",
     "Returns index of substring (-1 if not found)", NULL, 0},
    {"substring", XLSP_SYM_FUNCTION, "fn(s: string, start: int, end?: int): string",
     "Returns substring", NULL, 0},
    {"replace", XLSP_SYM_FUNCTION, "fn(s: string, old: string, new: string): string",
     "Replaces occurrences", NULL, 0},
    {"repeat", XLSP_SYM_FUNCTION, "fn(s: string, count: int): string", "Repeats string", NULL, 0},
    {"reverse", XLSP_SYM_FUNCTION, "fn(s: string): string", "Reverses string", NULL, 0},
    {"charAt", XLSP_SYM_FUNCTION, "fn(s: string, index: int): string", "Returns character at index",
     NULL, 0},
    {"charCode", XLSP_SYM_FUNCTION, "fn(s: string, index: int): int",
     "Returns character code at index", NULL, 0},
    {"fromCharCode", XLSP_SYM_FUNCTION, "fn(code: int): string",
     "Creates string from character code", NULL, 0},
    {"padLeft", XLSP_SYM_FUNCTION, "fn(s: string, len: int, pad?: string): string",
     "Pads string on left", NULL, 0},
    {"padRight", XLSP_SYM_FUNCTION, "fn(s: string, len: int, pad?: string): string",
     "Pads string on right", NULL, 0},
};

// ============================================================================
// io module
// ============================================================================

static const XlspSymbolInfo io_symbols[] = {
    {"readFile", XLSP_SYM_FUNCTION, "fn(path: string): string", "Reads entire file as string", NULL,
     0},
    {"writeFile", XLSP_SYM_FUNCTION, "fn(path: string, content: string)", "Writes string to file",
     NULL, 0},
    {"appendFile", XLSP_SYM_FUNCTION, "fn(path: string, content: string)", "Appends string to file",
     NULL, 0},
    {"exists", XLSP_SYM_FUNCTION, "fn(path: string): bool", "Checks if path exists", NULL, 0},
    {"isFile", XLSP_SYM_FUNCTION, "fn(path: string): bool", "Checks if path is a file", NULL, 0},
    {"isDir", XLSP_SYM_FUNCTION, "fn(path: string): bool", "Checks if path is a directory", NULL,
     0},
    {"fileSize", XLSP_SYM_FUNCTION, "fn(path: string): int", "Returns file size in bytes", NULL, 0},
    {"remove", XLSP_SYM_FUNCTION, "fn(path: string)", "Removes file or empty directory", NULL, 0},
    {"rename", XLSP_SYM_FUNCTION, "fn(old: string, new: string)", "Renames file or directory", NULL,
     0},
    {"mkdir", XLSP_SYM_FUNCTION, "fn(path: string)", "Creates directory", NULL, 0},
    {"mkdirp", XLSP_SYM_FUNCTION, "fn(path: string)", "Creates directory recursively", NULL, 0},
    {"readDir", XLSP_SYM_FUNCTION, "fn(path: string): Array<string>", "Lists directory contents",
     NULL, 0},
    {"readDirRecursive", XLSP_SYM_FUNCTION, "fn(path: string): Array<string>",
     "Lists directory contents recursively", NULL, 0},
    {"cwd", XLSP_SYM_FUNCTION, "fn(): string", "Returns current working directory", NULL, 0},
    {"chdir", XLSP_SYM_FUNCTION, "fn(path: string)", "Changes current directory", NULL, 0},
    {"copyFile", XLSP_SYM_FUNCTION, "fn(src: string, dst: string)", "Copies file", NULL, 0},
    {"readLines", XLSP_SYM_FUNCTION, "fn(path: string): Array<string>",
     "Reads file as array of lines", NULL, 0},
    {"stat", XLSP_SYM_FUNCTION, "fn(path: string): Json", "Returns file/directory info", NULL, 0},
    {"removeAll", XLSP_SYM_FUNCTION, "fn(path: string)", "Removes directory recursively", NULL, 0},
    {"chmod", XLSP_SYM_FUNCTION, "fn(path: string, mode: int)", "Changes file permissions", NULL,
     0},
    {"symlink", XLSP_SYM_FUNCTION, "fn(target: string, link: string)", "Creates symbolic link",
     NULL, 0},
    {"realpath", XLSP_SYM_FUNCTION, "fn(path: string): string", "Returns real path", NULL, 0},
    {"tempFile", XLSP_SYM_FUNCTION, "fn(): string", "Creates temporary file", NULL, 0},
    {"tempDir", XLSP_SYM_FUNCTION, "fn(): string", "Creates temporary directory", NULL, 0},
};

// ============================================================================
// csv module
// ============================================================================

static const XlspSymbolInfo csv_symbols[] = {
    {"parse", XLSP_SYM_FUNCTION, "fn(text: string): Array<Array<string>>", "Parses CSV text", NULL,
     0},
    {"parseDetailed", XLSP_SYM_FUNCTION, "fn(text: string): Json", "Parses CSV with headers", NULL,
     0},
    {"parseTsv", XLSP_SYM_FUNCTION, "fn(text: string): Array<Array<string>>", "Parses TSV text",
     NULL, 0},
    {"parseAuto", XLSP_SYM_FUNCTION, "fn(text: string): Array<Array<string>>",
     "Parses with auto-detected delimiter", NULL, 0},
    {"stringify", XLSP_SYM_FUNCTION, "fn(data: Array): string", "Converts to CSV string", NULL, 0},
    {"parseFile", XLSP_SYM_FUNCTION, "fn(path: string): Array<Array<string>>", "Parses CSV file",
     NULL, 0},
    {"writeFile", XLSP_SYM_FUNCTION, "fn(path: string, data: Array)", "Writes CSV file", NULL, 0},
};

// ============================================================================
// compress module
// ============================================================================

static const XlspSymbolInfo compress_symbols[] = {
    {"gzip", XLSP_SYM_FUNCTION, "fn(data: Bytes): Bytes", "Compresses with gzip", NULL, 0},
    {"gunzip", XLSP_SYM_FUNCTION, "fn(data: Bytes): Bytes", "Decompresses gzip", NULL, 0},
    {"isGzip", XLSP_SYM_FUNCTION, "fn(data: Bytes): bool", "Checks if data is gzip", NULL, 0},
    {"deflate", XLSP_SYM_FUNCTION, "fn(data: Bytes): Bytes", "Compresses with deflate", NULL, 0},
    {"inflate", XLSP_SYM_FUNCTION, "fn(data: Bytes): Bytes", "Decompresses deflate", NULL, 0},
    {"zlibCompress", XLSP_SYM_FUNCTION, "fn(data: Bytes): Bytes", "Compresses with zlib", NULL, 0},
    {"zlibDecompress", XLSP_SYM_FUNCTION, "fn(data: Bytes): Bytes", "Decompresses zlib", NULL, 0},
    {"crc32", XLSP_SYM_FUNCTION, "fn(data: Bytes): int", "Computes CRC32 checksum", NULL, 0},
    {"adler32", XLSP_SYM_FUNCTION, "fn(data: Bytes): int", "Computes Adler32 checksum", NULL, 0},
};

// ============================================================================
// encoding module
// ============================================================================

static const XlspSymbolInfo encoding_symbols[] = {
    {"hexEncode", XLSP_SYM_FUNCTION, "fn(data: Bytes): string", "Encodes bytes to hex string", NULL,
     0},
    {"hexDecode", XLSP_SYM_FUNCTION, "fn(hex: string): Bytes", "Decodes hex string to bytes", NULL,
     0},
    {"hexValid", XLSP_SYM_FUNCTION, "fn(hex: string): bool", "Checks if valid hex string", NULL, 0},
    {"utf8Valid", XLSP_SYM_FUNCTION, "fn(s: string): bool", "Checks if valid UTF-8", NULL, 0},
    {"utf8Count", XLSP_SYM_FUNCTION, "fn(s: string): int", "Counts UTF-8 characters", NULL, 0},
    {"utf16Encode", XLSP_SYM_FUNCTION, "fn(s: string): Bytes", "Encodes string to UTF-16", NULL, 0},
    {"utf16Decode", XLSP_SYM_FUNCTION, "fn(data: Bytes): string", "Decodes UTF-16 to string", NULL,
     0},
};

// ============================================================================
// gc module
// ============================================================================

static const XlspSymbolInfo gc_symbols[] = {
    // Control
    {"collect", XLSP_SYM_FUNCTION, "fn()", "Forces a full GC cycle on current coroutine", NULL, 0},
    {"step", XLSP_SYM_FUNCTION, "fn()", "Performs one incremental GC step", NULL, 0},
    {"disable", XLSP_SYM_FUNCTION, "fn()", "Disables automatic GC (can be nested)", NULL, 0},
    {"enable", XLSP_SYM_FUNCTION, "fn()", "Enables automatic GC (decrements disable counter)", NULL,
     0},
    {"isrunning", XLSP_SYM_FUNCTION, "fn(): bool", "Returns true if GC is enabled", NULL, 0},
    // Statistics
    {"count", XLSP_SYM_FUNCTION, "fn(): float", "Returns memory usage in KB", NULL, 0},
    {"countb", XLSP_SYM_FUNCTION, "fn(): int", "Returns memory usage in bytes", NULL, 0},
    {"objects", XLSP_SYM_FUNCTION, "fn(): int", "Returns approximate object count", NULL, 0},
    {"gccount", XLSP_SYM_FUNCTION, "fn(): int", "Returns number of completed GC cycles", NULL, 0},
    {"debt", XLSP_SYM_FUNCTION, "fn(): int", "Returns current GC debt (triggers GC when positive)",
     NULL, 0},
    {"state", XLSP_SYM_FUNCTION, "fn(): string",
     "Returns GC state: PAUSE, PROPAGATE, ATOMIC, SWEEP, or FINALIZE", NULL, 0},
    {"info", XLSP_SYM_FUNCTION, "fn(): Map",
     "Returns comprehensive GC info including generational stats", NULL, 0},
    // Tuning
    {"setpause", XLSP_SYM_FUNCTION, "fn(n: int): int",
     "Sets GC pause multiplier (default 100), returns old value", NULL, 0},
    {"setstepmul", XLSP_SYM_FUNCTION, "fn(n: int): int",
     "Sets GC step multiplier (default 200), returns old value", NULL, 0},
};

// ============================================================================
// toml module
// ============================================================================

static const XlspSymbolInfo toml_symbols[] = {
    {"parse", XLSP_SYM_FUNCTION, "fn(text: string): Json", "Parses TOML text", NULL, 0},
    {"parseStrict", XLSP_SYM_FUNCTION, "fn(text: string): Json", "Parses TOML strictly", NULL, 0},
    {"stringify", XLSP_SYM_FUNCTION, "fn(obj: Json): string", "Converts to TOML string", NULL, 0},
    {"parseFile", XLSP_SYM_FUNCTION, "fn(path: string): Json", "Parses TOML file", NULL, 0},
    {"writeFile", XLSP_SYM_FUNCTION, "fn(path: string, obj: Json)", "Writes TOML file", NULL, 0},
};

// ============================================================================
// url module
// ============================================================================

static const XlspSymbolInfo url_symbols[] = {
    {"parse", XLSP_SYM_FUNCTION, "fn(url: string): Json", "Parses URL into components", NULL, 0},
    {"encode", XLSP_SYM_FUNCTION, "fn(s: string): string", "URL-encodes string", NULL, 0},
    {"decode", XLSP_SYM_FUNCTION, "fn(s: string): string", "URL-decodes string", NULL, 0},
};

// ============================================================================
// xml module
// ============================================================================

static const XlspSymbolInfo xml_symbols[] = {
    {"parse", XLSP_SYM_FUNCTION, "fn(text: string): Json", "Parses XML text", NULL, 0},
    {"parseDetailed", XLSP_SYM_FUNCTION, "fn(text: string): Json", "Parses XML with attributes",
     NULL, 0},
    {"parseFile", XLSP_SYM_FUNCTION, "fn(path: string): Json", "Parses XML file", NULL, 0},
    {"stringify", XLSP_SYM_FUNCTION, "fn(obj: Json): string", "Converts to XML string", NULL, 0},
    {"writeFile", XLSP_SYM_FUNCTION, "fn(path: string, obj: Json)", "Writes XML file", NULL, 0},
    {"document", XLSP_SYM_FUNCTION, "fn(): XmlNode", "Creates XML document", NULL, 0},
    {"element", XLSP_SYM_FUNCTION, "fn(name: string): XmlNode", "Creates XML element", NULL, 0},
    {"text", XLSP_SYM_FUNCTION, "fn(content: string): XmlNode", "Creates text node", NULL, 0},
    {"comment", XLSP_SYM_FUNCTION, "fn(content: string): XmlNode", "Creates comment node", NULL, 0},
};

// ============================================================================
// yaml module
// ============================================================================

static const XlspSymbolInfo yaml_symbols[] = {
    {"parse", XLSP_SYM_FUNCTION, "fn(text: string): Json", "Parses YAML text", NULL, 0},
    {"parseStrict", XLSP_SYM_FUNCTION, "fn(text: string): Json", "Parses YAML strictly", NULL, 0},
    {"parseAll", XLSP_SYM_FUNCTION, "fn(text: string): Array<Json>", "Parses multi-document YAML",
     NULL, 0},
    {"stringify", XLSP_SYM_FUNCTION, "fn(obj: Json): string", "Converts to YAML string", NULL, 0},
    {"parseFile", XLSP_SYM_FUNCTION, "fn(path: string): Json", "Parses YAML file", NULL, 0},
    {"writeFile", XLSP_SYM_FUNCTION, "fn(path: string, obj: Json)", "Writes YAML file", NULL, 0},
};

// ============================================================================
// cluster module
// ============================================================================

static const XlspSymbolInfo cluster_symbols[] = {
    {"start", XLSP_SYM_FUNCTION, "fn(config: Json): void", "Start cluster node with configuration",
     NULL, 0},
    {"join", XLSP_SYM_FUNCTION, "fn(host: string, port: int): bool", "Join an existing cluster",
     NULL, 0},
    {"self", XLSP_SYM_FUNCTION, "fn(): string", "Get own node name", NULL, 0},
    {"nodes", XLSP_SYM_FUNCTION, "fn(): Array<string>", "List all cluster nodes", NULL, 0},
    {"channel", XLSP_SYM_FUNCTION, "fn(name: string): Channel",
     "Get or create a distributed named channel", NULL, 0},
    {"serve", XLSP_SYM_FUNCTION, "fn(name: string, handler: fn): void",
     "Register a service handler", NULL, 0},
    {"call", XLSP_SYM_FUNCTION, "fn(node: string, service: string, data: Json): Json",
     "Call a remote service (blocking)", NULL, 0},
    {"reply", XLSP_SYM_FUNCTION, "fn(req: Json, result: Json): void", "Reply to a service request",
     NULL, 0},
    {"monitor", XLSP_SYM_FUNCTION, "fn(node: string): Channel",
     "Monitor node health, returns Channel", NULL, 0},
    {"stop", XLSP_SYM_FUNCTION, "fn(): void", "Stop cluster node", NULL, 0},
};

// ============================================================================
// net module
// ============================================================================

static const XlspSymbolInfo net_symbols[] = {
    {"dial", XLSP_SYM_FUNCTION, "fn(host: string, port: int): Connection", "Connects to TCP server",
     NULL, 0},
    {"listen", XLSP_SYM_FUNCTION, "fn(port: int): Listener", "Creates TCP listener", NULL, 0},
    {"accept", XLSP_SYM_FUNCTION, "fn(listener: Listener): Connection",
     "Accepts incoming connection", NULL, 0},
    {"close", XLSP_SYM_FUNCTION, "fn(conn: Connection)", "Closes connection", NULL, 0},
    {"lookup", XLSP_SYM_FUNCTION, "fn(host: string): Array<string>", "DNS lookup", NULL, 0},
};

// ============================================================================
// ws module (WebSocket)
// ============================================================================

static const XlspSymbolInfo ws_symbols[] = {
    // Client API
    {"connect", XLSP_SYM_FUNCTION, "fn(url: string, options?: Json): Connection",
     "Connect to WebSocket server, returns connection object", NULL, 0},
    {"send", XLSP_SYM_FUNCTION, "fn(conn: Connection, message: string, binary?: bool): bool",
     "Send message on WebSocket connection", NULL, 0},
    {"recv", XLSP_SYM_FUNCTION, "fn(conn: Connection, timeout?: int): Message",
     "Receive message from WebSocket connection (blocking)", NULL, 0},
    {"close", XLSP_SYM_FUNCTION, "fn(conn: Connection, code?: int, reason?: string): bool",
     "Close WebSocket connection", NULL, 0},
    {"ping", XLSP_SYM_FUNCTION, "fn(conn: Connection): bool", "Send ping frame", NULL, 0},
    {"state", XLSP_SYM_FUNCTION, "fn(conn: Connection): string",
     "Get connection state: connecting, open, closing, closed", NULL, 0},
    {"isOpen", XLSP_SYM_FUNCTION, "fn(conn: Connection): bool", "Check if connection is open", NULL,
     0},
    {"hasError", XLSP_SYM_FUNCTION, "fn(conn: Connection): bool", "Check if connection has error",
     NULL, 0},
    // Server API
    {"serve", XLSP_SYM_FUNCTION, "fn(port: int, handler: fn(Connection)): bool",
     "Start WebSocket server with coroutine-per-connection model", NULL, 0},
    {"stopServer", XLSP_SYM_FUNCTION, "fn(): void", "Stop WebSocket server", NULL, 0},
    {"isServerRunning", XLSP_SYM_FUNCTION, "fn(): bool", "Check if WebSocket server is running",
     NULL, 0},
};

// ============================================================================
// All modules
// ============================================================================

static const XlspModuleInfo stdlib_modules[] = {
    {"time", "Time and date utilities", time_symbols,
     sizeof(time_symbols) / sizeof(time_symbols[0])},
    {"json", "JSON parsing and serialization", json_symbols,
     sizeof(json_symbols) / sizeof(json_symbols[0])},
    {"http", "HTTP client and server", http_symbols,
     sizeof(http_symbols) / sizeof(http_symbols[0])},
    {"regex", "Regular expression methods (via /pattern/ syntax)", regex_symbols,
     sizeof(regex_symbols) / sizeof(regex_symbols[0])},
    {"log", "Logging utilities", log_symbols, sizeof(log_symbols) / sizeof(log_symbols[0])},
    {"math", "Mathematical functions and constants", math_symbols,
     sizeof(math_symbols) / sizeof(math_symbols[0])},
    {"os", "Operating system interface", os_symbols, sizeof(os_symbols) / sizeof(os_symbols[0])},
    {"fs", "File system operations", fs_symbols, sizeof(fs_symbols) / sizeof(fs_symbols[0])},
    {"crypto", "Cryptographic functions", crypto_symbols,
     sizeof(crypto_symbols) / sizeof(crypto_symbols[0])},
    {"base64", "Base64 encoding/decoding", base64_symbols,
     sizeof(base64_symbols) / sizeof(base64_symbols[0])},
    {"datetime", "Date and time manipulation", datetime_symbols,
     sizeof(datetime_symbols) / sizeof(datetime_symbols[0])},
    {"path", "Path manipulation utilities", path_symbols,
     sizeof(path_symbols) / sizeof(path_symbols[0])},
    {"string", "String manipulation utilities", string_symbols,
     sizeof(string_symbols) / sizeof(string_symbols[0])},
    {"io", "File and I/O operations", io_symbols, sizeof(io_symbols) / sizeof(io_symbols[0])},
    {"csv", "CSV parsing and generation", csv_symbols,
     sizeof(csv_symbols) / sizeof(csv_symbols[0])},
    {"compress", "Compression utilities (gzip, zlib)", compress_symbols,
     sizeof(compress_symbols) / sizeof(compress_symbols[0])},
    {"encoding", "Character encoding utilities", encoding_symbols,
     sizeof(encoding_symbols) / sizeof(encoding_symbols[0])},
    {"gc", "Garbage collection control", gc_symbols, sizeof(gc_symbols) / sizeof(gc_symbols[0])},
    {"toml", "TOML parsing and generation", toml_symbols,
     sizeof(toml_symbols) / sizeof(toml_symbols[0])},
    {"url", "URL parsing and encoding", url_symbols, sizeof(url_symbols) / sizeof(url_symbols[0])},
    {"xml", "XML parsing and generation", xml_symbols,
     sizeof(xml_symbols) / sizeof(xml_symbols[0])},
    {"yaml", "YAML parsing and generation", yaml_symbols,
     sizeof(yaml_symbols) / sizeof(yaml_symbols[0])},
    {"net", "Low-level network operations", net_symbols,
     sizeof(net_symbols) / sizeof(net_symbols[0])},
    {"ws", "WebSocket client and server", ws_symbols, sizeof(ws_symbols) / sizeof(ws_symbols[0])},
    {"cluster", "Distributed cluster communication", cluster_symbols,
     sizeof(cluster_symbols) / sizeof(cluster_symbols[0])},
};

static const int stdlib_module_count = sizeof(stdlib_modules) / sizeof(stdlib_modules[0]);

// ============================================================================
// API Implementation
// ============================================================================

const XlspModuleInfo *xlsp_stdlib_get_modules(int *count) {
    if (count)
        *count = stdlib_module_count;
    return stdlib_modules;
}

const XlspModuleInfo *xlsp_stdlib_find_module(const char *name) {
    if (!name)
        return NULL;

    for (int i = 0; i < stdlib_module_count; i++) {
        if (strcmp(stdlib_modules[i].name, name) == 0) {
            return &stdlib_modules[i];
        }
    }
    return NULL;
}

const XlspSymbolInfo *xlsp_stdlib_find_symbol(const XlspModuleInfo *module, const char *name) {
    if (!module || !name)
        return NULL;

    for (int i = 0; i < module->symbol_count; i++) {
        if (strcmp(module->symbols[i].name, name) == 0) {
            return &module->symbols[i];
        }
    }
    return NULL;
}
