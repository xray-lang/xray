/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xregex_binding.h - Xray regex module binding
 *
 * KEY CONCEPT:
 *   Provides regex functionality for Xray scripts.
 *
 * PERFORMANCE (based on RE2 design):
 *   - Linear time matching guarantee, no backtracking trap
 *   - Thompson NFA algorithm
 *   - Controlled memory usage
 *
 * USAGE EXAMPLE:
 *   import regex
 *
 *   // Compile regex
 *   let re = regex.compile("\\d+")
 *
 *   // Test match
 *   if (regex.test(re, "abc123")) {
 *       print("found!")
 *   }
 *
 *   // Get match result
 *   let m = regex.match(re, "abc123def")
 *   print(m.start, m.end)  // 3 6
 *
 *   // Capture groups
 *   let re2 = regex.compile("(\\w+)@(\\w+)")
 *   let m2 = regex.match(re2, "user@host")
 *   print(m2.groups[1])  // "user"
 *   print(m2.groups[2])  // "host"
 *
 *   // Replace
 *   let result = regex.replace(re, "a1b2c3", "X")  // "aXbXcX"
 *
 *   // Split
 *   let parts = regex.split(regex.compile(",\\s*"), "a, b, c")  // ["a", "b", "c"]
 */

#ifndef XREGEX_BINDING_H
#define XREGEX_BINDING_H

#include "xregex.h"
#include "../../src/base/xdefs.h"
#include "../../src/runtime/value/xvalue.h"

struct XrModule;

/*
 * Load regex module
 *
 * Provided functions:
 *   - compile(pattern)           Compile regex, return Regex object
 *   - compile(pattern, flags)    Compile with flags ("i"=ignorecase, "m"=multiline, "s"=dotall)
 *   - test(re, text)             Test if matches, return bool
 *   - match(re, text)            Find match, return Match object or null
 *   - fullMatch(re, text)        Full match (anchored to start and end)
 *   - replace(re, text, repl)    Replace first match
 *   - replaceAll(re, text, repl) Replace all matches
 *   - split(re, text)            Split by pattern
 *   - escape(text)               Escape special characters
 *
 * Match object properties:
 *   - start                      Match start position
 *   - end                        Match end position
 *   - text                       Matched text
 *   - groups                     Capture group array (groups[0] is full match)
 */
XR_FUNC struct XrModule *xr_load_module_regex(XrayIsolate *isolate);

/*
 * Wrap XrRegex as XrValue (GC-managed XrRegexObject)
 * @param X  Isolate context
 * @param re Regex object pointer
 * @return Wrapped XrValue
 */
XR_FUNC XrValue xr_regex_wrap(XrayIsolate *X, XrRegex *re);

/*
 * Register the Regex XrClass so regex literals and regex.compile(...)
 * results can dispatch instance methods. Called unconditionally from
 * xr_prelude_register_all_native_types during isolate init.
 */
void xr_regex_register_native_type(XrayIsolate *isolate);

// Check if value is a Regex object
XR_FUNC bool xr_value_is_regex(XrValue v);

// Get Regex pointer
XR_FUNC XrRegex *xr_value_to_regex(XrValue v);

/*
 * Build a Json match result with shape { start, end, text, groups }.
 * Lifted from a static helper so stdlib/regex/regex_methods.c can
 * reuse the exact same shape used by the legacy native-type binding.
 */
XR_FUNC XrValue xr_regex_make_match_object(XrayIsolate *isolate, const char *text, XrMatch *match);

#endif  // XREGEX_BINDING_H
