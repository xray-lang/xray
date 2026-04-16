/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xsemver.c - Semantic versioning parser (SemVer 2.0.0)
 *
 * KEY CONCEPT:
 *   Parse and compare semantic versions (major.minor.patch-prerelease+build).
 *   Supports version constraints like ^1.2.3, ~1.2.0, >=1.0.0.
 */

#include "xsemver.h"
#include "../base/xchecks.h"
#include "../base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <limits.h>

static const char* skip_whitespace(const char *s) {
    while (*s && isspace(*s)) s++;
    return s;
}

/*
 * Parse a non-negative integer with overflow checking.
 * Returns pointer to first non-digit character.
 * Sets *out to -1 on parse failure or overflow.
 */
static const char* parse_number(const char *s, int *out) {
    if (!isdigit(*s)) {
        *out = -1;
        return s;
    }
    
    // Prevent leading zeros (except for "0" itself)
    if (*s == '0' && isdigit(*(s + 1))) {
        *out = -1;  // Invalid: leading zeros not allowed in semver
        return s;
    }
    
    long long val = 0;
    const long long max_val = INT32_MAX;
    
    while (isdigit(*s)) {
        val = val * 10 + (*s - '0');
        if (val > max_val) {
            *out = -1;  // Overflow
            // Skip remaining digits
            while (isdigit(*s)) s++;
            return s;
        }
        s++;
    }
    
    *out = (int)val;
    return s;
}

static char* parse_identifier(const char **s, char stop_char) {
    const char *start = *s;
    
    // Skip valid characters: letters, digits, dots, hyphens
    while (**s && **s != stop_char && **s != '+') {
        if (!isalnum(**s) && **s != '.' && **s != '-') {
            break;
        }
        (*s)++;
    }
    
    if (*s == start) {
        return NULL;
    }
    
    size_t len = *s - start;
    char *result = (char*)xr_malloc(len + 1);
    if (result) {
        memcpy(result, start, len);
        result[len] = '\0';
    }
    return result;
}

/*
 * Compare prerelease identifiers.
 * Rules:
 * - No prerelease > has prerelease
 * - Compare by dot-separated segments
 * - Numeric segments compared numerically, others lexically
 */
static int compare_prerelease(const char *a, const char *b) {
    // No prerelease > has prerelease
    if (!a && !b) return 0;
    if (!a) return 1;   // a has no prerelease, a > b
    if (!b) return -1;  // b has no prerelease, b > a
    
    // Compare segment by segment
    while (*a && *b) {
        // Extract current segment
        const char *a_start = a;
        const char *b_start = b;
        
        while (*a && *a != '.') a++;
        while (*b && *b != '.') b++;
        
        size_t a_len = a - a_start;
        size_t b_len = b - b_start;
        
        // Check if pure numeric
        bool a_numeric = true, b_numeric = true;
        for (size_t i = 0; i < a_len; i++) {
            if (!isdigit(a_start[i])) { a_numeric = false; break; }
        }
        for (size_t i = 0; i < b_len; i++) {
            if (!isdigit(b_start[i])) { b_numeric = false; break; }
        }
        
        int cmp;
        if (a_numeric && b_numeric) {
            // Numeric comparison
            int a_num = atoi(a_start);
            int b_num = atoi(b_start);
            cmp = a_num - b_num;
        } else {
            // Lexicographic comparison
            size_t min_len = a_len < b_len ? a_len : b_len;
            cmp = memcmp(a_start, b_start, min_len);
            if (cmp == 0) {
                cmp = (int)a_len - (int)b_len;
            }
        }
        
        if (cmp != 0) return cmp;
        
        // Skip dot
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    
    // When segment count differs, fewer segments means smaller
    if (*a) return 1;
    if (*b) return -1;
    return 0;
}

/* ========== Version API Implementation ========== */

bool xr_semver_parse(const char *str, XrSemVer *ver) {
    if (!str || !ver) return false;
    
    // Initialize
    memset(ver, 0, sizeof(XrSemVer));
    
    const char *p = skip_whitespace(str);
    
    // Skip optional 'v' prefix
    if (*p == 'v' || *p == 'V') {
        p++;
    }
    
    // Parse major version
    p = parse_number(p, &ver->major);
    if (ver->major < 0) return false;
    
    // Parse minor version
    if (*p != '.') {
        // Major only is valid: 1 -> 1.0.0
        ver->minor = 0;
        ver->patch = 0;
        return true;
    }
    p++;  // Skip '.'
    
    p = parse_number(p, &ver->minor);
    if (ver->minor < 0) return false;
    
    // Parse patch version
    if (*p != '.') {
        // 1.2 -> 1.2.0
        ver->patch = 0;
    } else {
        p++;  // Skip '.'
        p = parse_number(p, &ver->patch);
        if (ver->patch < 0) return false;
    }
    
    // Parse prerelease identifier
    if (*p == '-') {
        p++;
        ver->prerelease = parse_identifier(&p, '+');
    }
    
    // Parse build metadata
    if (*p == '+') {
        p++;
        ver->build = parse_identifier(&p, '\0');
    }
    
    // Check if parsing is complete
    p = skip_whitespace(p);
    return *p == '\0';
}

void xr_semver_free(XrSemVer *ver) {
    if (!ver) return;
    
    if (ver->prerelease) {
        xr_free(ver->prerelease);
        ver->prerelease = NULL;
    }
    if (ver->build) {
        xr_free(ver->build);
        ver->build = NULL;
    }
}

int xr_semver_compare(const XrSemVer *a, const XrSemVer *b) {
    if (!a || !b) return 0;
    
    // Compare major version first (safe comparison, no overflow)
    if (a->major != b->major) {
        return (a->major > b->major) - (a->major < b->major);
    }
    
    // Then compare minor version
    if (a->minor != b->minor) {
        return (a->minor > b->minor) - (a->minor < b->minor);
    }
    
    // Then compare patch version
    if (a->patch != b->patch) {
        return (a->patch > b->patch) - (a->patch < b->patch);
    }
    
    // Finally compare prerelease identifier
    return compare_prerelease(a->prerelease, b->prerelease);
}

int xr_semver_to_string(const XrSemVer *ver, char *buf, int size) {
    if (!ver || !buf || size <= 0) return 0;
    
    int written;
    
    if (ver->prerelease && ver->build) {
        written = snprintf(buf, size, "%d.%d.%d-%s+%s",
            ver->major, ver->minor, ver->patch,
            ver->prerelease, ver->build);
    } else if (ver->prerelease) {
        written = snprintf(buf, size, "%d.%d.%d-%s",
            ver->major, ver->minor, ver->patch, ver->prerelease);
    } else if (ver->build) {
        written = snprintf(buf, size, "%d.%d.%d+%s",
            ver->major, ver->minor, ver->patch, ver->build);
    } else {
        written = snprintf(buf, size, "%d.%d.%d",
            ver->major, ver->minor, ver->patch);
    }
    
    return written < size ? written : size - 1;
}

bool xr_semver_is_valid(const char *str) {
    XR_DCHECK(str != NULL, "semver_is_valid: NULL str");
    XrSemVer ver;
    bool valid = xr_semver_parse(str, &ver);
    xr_semver_free(&ver);
    return valid;
}

/* ========== Constraint API Implementation ========== */

bool xr_constraint_parse(const char *str, XrVersionConstraint *constraint) {
    if (!str || !constraint) return false;
    
    memset(constraint, 0, sizeof(XrVersionConstraint));
    
    const char *p = skip_whitespace(str);
    
    // Parse operator
    if (*p == '^') {
        constraint->op = SEMVER_OP_CARET;
        p++;
    } else if (*p == '~') {
        constraint->op = SEMVER_OP_TILDE;
        p++;
    } else if (*p == '>' && *(p+1) == '=') {
        constraint->op = SEMVER_OP_GE;
        p += 2;
    } else if (*p == '<' && *(p+1) == '=') {
        constraint->op = SEMVER_OP_LE;
        p += 2;
    } else if (*p == '>') {
        constraint->op = SEMVER_OP_GT;
        p++;
    } else if (*p == '<') {
        constraint->op = SEMVER_OP_LT;
        p++;
    } else if (*p == '=') {
        constraint->op = SEMVER_OP_EQ;
        p++;
    } else if (*p == '*') {
        constraint->op = SEMVER_OP_ANY;
        p++;
        p = skip_whitespace(p);
        return *p == '\0';
    } else {
        // No prefix, default to exact match
        constraint->op = SEMVER_OP_EQ;
    }
    
    // Skip possible whitespace
    p = skip_whitespace(p);
    
    // Parse version
    return xr_semver_parse(p, &constraint->version);
}

void xr_constraint_free(XrVersionConstraint *constraint) {
    if (!constraint) return;
    xr_semver_free(&constraint->version);
}

bool xr_constraint_matches(const XrSemVer *ver, const XrVersionConstraint *constraint) {
    if (!ver || !constraint) return false;
    
    int cmp = xr_semver_compare(ver, &constraint->version);
    
    switch (constraint->op) {
        case SEMVER_OP_ANY:
            return true;
            
        case SEMVER_OP_EQ:
            return cmp == 0;
            
        case SEMVER_OP_GT:
            return cmp > 0;
            
        case SEMVER_OP_GE:
            return cmp >= 0;
            
        case SEMVER_OP_LT:
            return cmp < 0;
            
        case SEMVER_OP_LE:
            return cmp <= 0;
            
        case SEMVER_OP_CARET: {
            // ^1.2.3 means >=1.2.3 and <2.0.0 (major compatible)
            // ^0.2.3 means >=0.2.3 and <0.3.0 (minor compatible, 0.x special case)
            // ^0.0.3 means =0.0.3 (exact match, 0.0.x special case)
            if (cmp < 0) return false;  // Must be >= base version
            
            const XrSemVer *base = &constraint->version;
            
            if (base->major == 0) {
                if (base->minor == 0) {
                    // ^0.0.x exact match patch
                    return ver->major == 0 && ver->minor == 0 && 
                           ver->patch == base->patch;
                }
                // ^0.x.y minor compatible
                return ver->major == 0 && ver->minor == base->minor;
            }
            
            // ^x.y.z major compatible
            return ver->major == base->major;
        }
            
        case SEMVER_OP_TILDE: {
            // ~1.2.3 means >=1.2.3 and <1.3.0 (patch updates)
            if (cmp < 0) return false;  // Must be >= base version
            
            const XrSemVer *base = &constraint->version;
            
            // Major and minor must be the same
            return ver->major == base->major && ver->minor == base->minor;
        }
    }
    
    return false;
}

int xr_constraint_to_string(const XrVersionConstraint *constraint, char *buf, int size) {
    if (!constraint || !buf || size <= 0) return 0;
    
    const char *op_str = "";
    switch (constraint->op) {
        case SEMVER_OP_EQ:    op_str = ""; break;
        case SEMVER_OP_GT:    op_str = ">"; break;
        case SEMVER_OP_GE:    op_str = ">="; break;
        case SEMVER_OP_LT:    op_str = "<"; break;
        case SEMVER_OP_LE:    op_str = "<="; break;
        case SEMVER_OP_CARET: op_str = "^"; break;
        case SEMVER_OP_TILDE: op_str = "~"; break;
        case SEMVER_OP_ANY:   return snprintf(buf, size, "*");
    }
    
    char ver_buf[64];
    xr_semver_to_string(&constraint->version, ver_buf, sizeof(ver_buf));
    
    return snprintf(buf, size, "%s%s", op_str, ver_buf);
}

int xr_semver_select_best(const XrSemVer *versions, int count, 
                          const XrVersionConstraint *constraint) {
    if (!versions || count <= 0 || !constraint) return -1;
    
    int best_idx = -1;
    
    for (int i = 0; i < count; i++) {
        if (xr_constraint_matches(&versions[i], constraint)) {
            if (best_idx < 0 || 
                xr_semver_compare(&versions[i], &versions[best_idx]) > 0) {
                best_idx = i;
            }
        }
    }
    
    return best_idx;
}
