/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * yaml_scanner.c - YAML lexical scanner
 *
 * KEY CONCEPT:
 *   High-performance lexical scanning using unified SWAR/SIMD library.
 */

#include "yaml_parser.h"
#include <string.h>
#include <ctype.h>

// Unified SWAR/SIMD utility library
#include "xswar.h"
#include "xsimd.h"

// ========== Fast Number Parsing (using unified SWAR library) ==========

bool yaml_fast_parse_uint(const char *s, size_t len, uint64_t *result) {
    return xr_swar_parse_uint(s, len, result);
}

bool yaml_fast_parse_int(const char *s, size_t len, int64_t *result) {
    return xr_swar_parse_int(s, len, result);
}

// ========== SIMD Scanning (using unified SIMD library) ==========

const char *yaml_simd_find_special(const char *s, size_t len) {
    // Find \n, :, #
    const char chars[3] = {'\n', ':', '#'};
    return xr_simd_find_any(s, len, chars, 3);
}

const char *yaml_simd_skip_ws(const char *s, size_t len) {
    return xr_simd_skip_ws(s, len);
}

const char *yaml_simd_find_newline(const char *s, size_t len) {
    return xr_simd_find_newline(s, len);
}

// ========== Scanning Helper Functions ===========

void yaml_skip_ws(YamlParser *p) {
    size_t remaining = p->end - p->ptr;
    if (remaining >= 16) {
        const char *new_ptr = yaml_simd_skip_ws(p->ptr, remaining);
        int skipped = (int) (new_ptr - p->ptr);
        p->col += skipped;
        p->ptr = new_ptr;
    }
    while (p->ptr < p->end && (*p->ptr == ' ' || *p->ptr == '\t')) {
        p->ptr++;
        p->col++;
    }
}

void yaml_skip_to_eol(YamlParser *p) {
    size_t remaining = p->end - p->ptr;
    if (remaining >= 16) {
        const char *found = yaml_simd_find_newline(p->ptr, remaining);
        // SIMD path used to bypass column tracking, so any error
        // reported after `# comment` on a long line had a stale `col`.
        // Track bytes skipped so diagnostics stay accurate.
        p->col += (int) (found - p->ptr);
        p->ptr = found;
    } else {
        while (p->ptr < p->end && *p->ptr != '\n' && *p->ptr != '\r') {
            p->ptr++;
            p->col++;
        }
    }
}

void yaml_skip_newline(YamlParser *p) {
    if (p->ptr < p->end && *p->ptr == '\r') {
        p->ptr++;
    }
    if (p->ptr < p->end && *p->ptr == '\n') {
        p->ptr++;
        p->line++;
        p->col = 1;
    }
}

void yaml_skip_empty_lines(YamlParser *p) {
    while (p->ptr < p->end) {
        const char *line_start = p->ptr;

        while (p->ptr < p->end && (*p->ptr == ' ' || *p->ptr == '\t')) {
            p->ptr++;
        }

        if (p->ptr < p->end && *p->ptr == '#') {
            yaml_skip_to_eol(p);
        }

        if (p->ptr < p->end && (*p->ptr == '\n' || *p->ptr == '\r')) {
            if (*p->ptr == '\r')
                p->ptr++;
            if (p->ptr < p->end && *p->ptr == '\n')
                p->ptr++;
            p->line++;
            p->col = 1;
        } else {
            p->ptr = line_start;
            break;
        }
    }
}

int yaml_count_indent(YamlParser *p) {
    const char *scan = p->ptr;
    int indent = 0;
    while (scan < p->end && *scan == ' ') {
        scan++;
        indent++;
    }
    return indent;
}

// Return true if the byte at (p->ptr + indent) is a tab, i.e. the user
// tried to indent the line with tabs (YAML 1.2 §6.1 forbids that).
// Caller decides whether to raise an error.
bool yaml_indent_has_tab(YamlParser *p, int indent) {
    const char *scan = p->ptr + indent;
    return scan < p->end && *scan == '\t';
}
