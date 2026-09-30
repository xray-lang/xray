/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xtoml.c - Pure C TOML v1.0.0 parser implementation
 *
 * KEY CONCEPT:
 *   Single-pass recursive descent parser producing an XrTomlValue DOM.
 *   No runtime dependency — uses only xr_malloc/xr_free from xmalloc.h.
 *   Supports: basic/multiline/literal strings, integers (dec/hex/oct/bin
 *   with underscores), floats (inf/nan), booleans, datetimes (as strings),
 *   arrays, inline tables, standard tables [t], array tables [[t]],
 *   dotted keys.
 */

#include "xtoml.h"
#include "xmalloc.h"
#include "../shared/xr_utf8_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>

typedef struct { char *text; size_t length; } TomlKey;

typedef struct {
    const char *data;
    size_t len;
    size_t pos;
    int line;
    int col;
    bool error;
    size_t allocation_remaining;
    size_t work_remaining;
    uint32_t depth, depth_limit;
    XrTomlParseStatus status;

    /* Reusable temp buffer for strings with escapes */
    char *buf;
    size_t buf_len;
    size_t buf_cap;
} TomlCtx;

/* ========== Allocators ========== */

static bool reserve_allocation(TomlCtx *p, size_t bytes) {
    if (p->error) return false;
    if (bytes > p->allocation_remaining) {
        p->error = true; p->status = XR_TOML_PARSE_LIMIT; return false;
    }
    p->allocation_remaining -= bytes;
    return true;
}
static void *toml_allocate(TomlCtx *p, size_t bytes, bool zero) {
    if (!reserve_allocation(p, bytes)) return NULL;
    void *result = zero ? xr_calloc(1, bytes) : xr_malloc(bytes);
    if (!result) { p->error = true; p->status = XR_TOML_PARSE_OUT_OF_MEMORY; }
    return result;
}
static void *toml_resize(TomlCtx *p, void *pointer, size_t bytes) {
    if (!reserve_allocation(p, bytes)) return NULL;
    void *result = xr_realloc(pointer, bytes);
    if (!result) { p->error = true; p->status = XR_TOML_PARSE_OUT_OF_MEMORY; }
    return result;
}
static char *toml_duplicate(TomlCtx *p, const char *text) {
    if (!reserve_allocation(p, strlen(text) + 1)) return NULL;
    char *result = xr_strdup(text);
    if (!result) { p->error = true; p->status = XR_TOML_PARSE_OUT_OF_MEMORY; }
    return result;
}



static bool parse_limit(TomlCtx *p) {
    p->error = true; p->status = XR_TOML_PARSE_LIMIT; return false;
}

static int grown_capacity(TomlCtx *p, int capacity, size_t item_size) {
    if (capacity <= 0 || capacity > INT_MAX / 2 || !item_size ||
        (size_t) capacity * 2 > SIZE_MAX / item_size) {
        parse_limit(p); return 0;
    }
    return capacity * 2;
}

static XrTomlValue *alloc_value(TomlCtx *p, XrTomlType type) {
    if (p->depth > p->depth_limit) { parse_limit(p); return NULL; }
    XrTomlValue *v = (XrTomlValue *) toml_allocate(p, sizeof(XrTomlValue), true);
    if (v) { v->type = type; v->depth = p->depth; }
    return v;
}

XR_FUNC void xtoml_free(XrTomlValue *v) {
    if (!v)
        return;
    switch (v->type) {
        case XR_TOML_STRING:
        case XR_TOML_DATETIME:
            xr_free(v->as.string);
            break;
        case XR_TOML_ARRAY:
            for (int i = 0; i < v->as.array.count; i++)
                xtoml_free(v->as.array.items[i]);
            xr_free(v->as.array.items);
            break;
        case XR_TOML_TABLE:
            for (int i = 0; i < v->as.table.count; i++) {
                xr_free(v->as.table.members[i].key);
                xtoml_free(v->as.table.members[i].value);
            }
            xr_free(v->as.table.members);
            break;
        default:
            break;
    }
    xr_free(v);
}

/* Consume owned text, including when node allocation fails. */
static XrTomlValue *new_string(TomlCtx *p, char *text, size_t length) {
    if (!text)
        return NULL;
    XrTomlValue *value = alloc_value(p, XR_TOML_STRING);
    if (!value) {
        xr_free(text);
        return NULL;
    }
    value->as.string = text;
    value->string_length = length;
    return value;
}

/* ========== Table Helpers ========== */

static XrTomlValue *new_table(TomlCtx *p) {
    XrTomlValue *t = alloc_value(p, XR_TOML_TABLE);
    if (!t)
        return NULL;
    t->as.table.capacity = 8;
    t->as.table.members =
        (XrTomlMember *) toml_allocate(p, (size_t) t->as.table.capacity * sizeof(XrTomlMember), true);
    if (!t->as.table.members) {
        xr_free(t);
        return NULL;
    }
    return t;
}

/* Find member by key. Returns index or -1. */
static int table_find(TomlCtx *p, XrTomlValue *t, const char *key, size_t length) {
    if (!t || t->type != XR_TOML_TABLE)
        return -1;
    for (int i = 0; i < t->as.table.count; i++) {
        if (p) {
            if (p->error || length == SIZE_MAX || length + 1 > p->work_remaining) {
                if (!p->error) parse_limit(p);
                return -1;
            }
            p->work_remaining -= length + 1;
        }
        if (t->as.table.members[i].key_length == length &&
            memcmp(t->as.table.members[i].key, key, length) == 0)
            return i;
    }
    return -1;
}

/* Insert an unassigned key. Ownership transfers only on success. */
static bool table_set(TomlCtx *p, XrTomlValue *t, const char *key, size_t length, XrTomlValue *val) {
    if (!t || t->type != XR_TOML_TABLE || !key)
        return false;

    if (table_find(p, t, key, length) >= 0)
        return false;

    if (t->as.table.count >= t->as.table.capacity) {
        int new_cap = grown_capacity(p, t->as.table.capacity, sizeof(XrTomlMember));
        if (!new_cap) return false;
        XrTomlMember *tmp = (XrTomlMember *) toml_resize(p, t->as.table.members,
                                                        (size_t) new_cap * sizeof(XrTomlMember));
        if (!tmp)
            return false;
        t->as.table.members = tmp;
        t->as.table.capacity = new_cap;
    }
    if (length == SIZE_MAX) return false;
    char *copy = toml_allocate(p, length + 1, false);
    if (!copy)
        return false;
    memcpy(copy, key, length); copy[length] = '\0';
    t->as.table.members[t->as.table.count].key = copy;
    t->as.table.members[t->as.table.count].key_length = length;
    t->as.table.members[t->as.table.count].value = val;
    t->as.table.count++;
    return true;
}

/* ========== Array Helpers ========== */

static XrTomlValue *new_array(TomlCtx *p) {
    XrTomlValue *a = alloc_value(p, XR_TOML_ARRAY);
    if (!a)
        return NULL;
    a->as.array.capacity = 8;
    a->as.array.items =
        (XrTomlValue **) toml_allocate(p, (size_t) a->as.array.capacity * sizeof(XrTomlValue *), true);
    if (!a->as.array.items) {
        xr_free(a);
        return NULL;
    }
    return a;
}

static bool array_push(TomlCtx *p, XrTomlValue *a, XrTomlValue *val) {
    if (!a || a->type != XR_TOML_ARRAY)
        return false;
    if (a->as.array.count >= a->as.array.capacity) {
        int new_cap = grown_capacity(p, a->as.array.capacity, sizeof(XrTomlValue *));
        if (!new_cap) return false;
        XrTomlValue **tmp = (XrTomlValue **) toml_resize(p, a->as.array.items,
                                                        (size_t) new_cap * sizeof(XrTomlValue *));
        if (!tmp)
            return false;
        a->as.array.items = tmp;
        a->as.array.capacity = new_cap;
    }
    a->as.array.items[a->as.array.count++] = val;
    return true;
}

/* ========== Parser Context ========== */

#define PEEK(p) ((p)->pos < (p)->len ? (p)->data[(p)->pos] : '\0')
#define AT_END(p) ((p)->pos >= (p)->len)
#define ADV(p)                                                                                     \
    do {                                                                                           \
        (p)->pos++;                                                                                \
        (p)->col++;                                                                                \
    } while (0)

static void ctx_init(TomlCtx *p, const char *data, size_t len) {
    memset(p, 0, sizeof(TomlCtx));
    p->data = data;
    p->len = len;
    p->line = 1;
    p->col = 1;
}

static void ctx_cleanup(TomlCtx *p) {
    xr_free(p->buf);
    p->buf = NULL;
}

/* ========== Buffer Helpers ========== */

static void buf_ensure(TomlCtx *p, size_t needed) {
    if (p->buf_cap >= needed)
        return;
    size_t new_cap = p->buf_cap ? p->buf_cap : 64;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) { new_cap = needed; break; }
        new_cap *= 2;
    }
    char *tmp = (char *) toml_resize(p, p->buf, new_cap);
    if (!tmp) {
        p->error = true;
        return;
    }
    p->buf = tmp;
    p->buf_cap = new_cap;
}

static void buf_reset(TomlCtx *p) {
    p->buf_len = 0;
}

static void buf_char(TomlCtx *p, char c) {
    if (p->buf_len > SIZE_MAX - 2) { parse_limit(p); return; }
    buf_ensure(p, p->buf_len + 2);
    if (!p->error)
        p->buf[p->buf_len++] = c;
}

static char *buf_dup(TomlCtx *p) {
    if (p->buf_len == SIZE_MAX) { parse_limit(p); return NULL; }
    buf_ensure(p, p->buf_len + 1);
    if (p->error)
        return NULL;
    p->buf[p->buf_len] = '\0';
    char *copy = toml_allocate(p, p->buf_len + 1, false);
    if (copy) memcpy(copy, p->buf, p->buf_len + 1);
    return copy;
}

/* ========== Skip Helpers ========== */

static void skip_ws(TomlCtx *p) {
    while (!AT_END(p) && (PEEK(p) == ' ' || PEEK(p) == '\t'))
        ADV(p);
}

static void skip_ws_nl(TomlCtx *p) {
    while (!AT_END(p)) {
        char c = PEEK(p);
        if (c == ' ' || c == '\t') {
            ADV(p);
            continue;
        }
        if (c == '\n') {
            ADV(p);
            p->line++;
            p->col = 1;
            continue;
        }
        if (c == '\r') {
            ADV(p);
            if (!AT_END(p) && PEEK(p) == '\n')
                ADV(p);
            p->line++;
            p->col = 1;
            continue;
        }
        if (c == '#') {
            while (!AT_END(p) && PEEK(p) != '\n')
                ADV(p);
            continue;
        }
        break;
    }
}

static void finish_line(TomlCtx *p) {
    skip_ws(p);
    if (!AT_END(p) && PEEK(p) == '#') {
        while (!AT_END(p) && PEEK(p) != '\n' && PEEK(p) != '\r')
            ADV(p);
    }
    if (!AT_END(p) && PEEK(p) != '\n' && PEEK(p) != '\r')
        p->error = true;
}

static bool is_bare_key_char(char c) {
    return isalnum((unsigned char) c) || c == '_' || c == '-';
}

/* ========== Forward Declarations ========== */

static XrTomlValue *parse_value(TomlCtx *p, uint32_t depth);
static void set_nested(TomlCtx *p, XrTomlValue *root, TomlKey *keys, int nkeys, XrTomlValue *val);

/* ========== UTF-8 Encode ========== */

static void utf8_encode(TomlCtx *p, unsigned int cp) {
    if (cp < 0x80) {
        buf_char(p, (char) cp);
    } else if (cp < 0x800) {
        buf_char(p, (char) (0xC0 | (cp >> 6)));
        buf_char(p, (char) (0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf_char(p, (char) (0xE0 | (cp >> 12)));
        buf_char(p, (char) (0x80 | ((cp >> 6) & 0x3F)));
        buf_char(p, (char) (0x80 | (cp & 0x3F)));
    } else {
        buf_char(p, (char) (0xF0 | (cp >> 18)));
        buf_char(p, (char) (0x80 | ((cp >> 12) & 0x3F)));
        buf_char(p, (char) (0x80 | ((cp >> 6) & 0x3F)));
        buf_char(p, (char) (0x80 | (cp & 0x3F)));
    }
}

/* ========== String Parsing ========== */

static bool parse_escape(TomlCtx *p, bool multiline) {
    if (AT_END(p)) goto invalid;
    char c = PEEK(p);
    ADV(p);
    const char *codes = "btnfr\"\\";
    const char decoded[] = {'\b', '\t', '\n', '\f', '\r', '"', '\\'};
    const char *match = c ? strchr(codes, c) : NULL;
    if (match) {
        buf_char(p, decoded[match - codes]);
        return !p->error;
    }
    if (c == 'u' || c == 'U') {
        unsigned digits = c == 'u' ? 4 : 8, cp = 0;
        if (p->len - p->pos < digits) goto invalid;
        for (unsigned i = 0; i < digits; ++i) {
            unsigned char h = (unsigned char) PEEK(p);
            if (!isxdigit(h)) goto invalid;
            ADV(p);
            cp = (cp << 4) | (h <= '9' ? h - '0' : (h <= 'F' ? h - 'A' + 10 : h - 'a' + 10));
        }
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) goto invalid;
        utf8_encode(p, cp);
        return !p->error;
    }
    if (!multiline) goto invalid;
    if (c == ' ' || c == '\t') {
        skip_ws(p);
        if (AT_END(p)) goto invalid;
        c = PEEK(p);
        ADV(p);
    }
    if (c != '\n' && c != '\r') goto invalid;
    if (c == '\r') {
        if (AT_END(p) || PEEK(p) != '\n') goto invalid;
        ADV(p);
    }
    ++p->line;
    p->col = 1;
    while (!AT_END(p)) {
        char next = PEEK(p);
        if (next != ' ' && next != '\t' && next != '\r' && next != '\n') break;
        ADV(p);
        if (next == '\n') { ++p->line; p->col = 1; }
    }
    return true;
invalid:
    p->error = true;
    return false;
}

/* Parse basic string (double-quoted, with escapes). */
static XrTomlValue *parse_basic_string(TomlCtx *p) {
    if (PEEK(p) != '"') {
        p->error = true;
        return NULL;
    }
    ADV(p);

    /* Check for multiline: """ */
    bool multiline = false;
    if (!AT_END(p) && PEEK(p) == '"') {
        ADV(p);
        if (!AT_END(p) && PEEK(p) == '"') {
            multiline = true;
            ADV(p);
            /* Skip first newline after opening """ */
            if (!AT_END(p) && PEEK(p) == '\n') {
                ADV(p);
                p->line++;
                p->col = 1;
            } else if (!AT_END(p) && PEEK(p) == '\r') {
                ADV(p);
                if (!AT_END(p) && PEEK(p) == '\n')
                    ADV(p);
                p->line++;
                p->col = 1;
            }
        } else {
            /* Empty string "" */
            return new_string(p, toml_duplicate(p, ""), 0);
        }
    }

    buf_reset(p);

    while (!AT_END(p)) {
        if (multiline) {
            if (PEEK(p) == '"' && p->pos + 2 < p->len && p->data[p->pos + 1] == '"' &&
                p->data[p->pos + 2] == '"') {
                p->pos += 3;
                p->col += 3;
                for (unsigned quotes = 0; quotes < 2 && !AT_END(p) && PEEK(p) == '"'; ++quotes) {
                    buf_char(p, '"');
                    ADV(p);
                }
                goto done;
            }
        } else {
            if (PEEK(p) == '"') {
                ADV(p);
                goto done;
            }
            if (PEEK(p) == '\n' || PEEK(p) == '\r') {
                p->error = true;
                return NULL;
            }
        }

        if (PEEK(p) == '\\') {
            ADV(p);
            if (!parse_escape(p, multiline)) return NULL;
        } else {
            if (PEEK(p) == '\n') {
                p->line++;
                p->col = 1;
            }
            buf_char(p, PEEK(p));
            ADV(p);
        }
    }
    /* Unterminated */
    p->error = true;
    return NULL;

done:
    return new_string(p, buf_dup(p), p->buf_len);
}

/* Parse literal string (single-quoted, no escapes). */
static XrTomlValue *parse_literal_string(TomlCtx *p) {
    if (PEEK(p) != '\'') {
        p->error = true;
        return NULL;
    }
    ADV(p);

    bool multiline = false;
    if (!AT_END(p) && PEEK(p) == '\'') {
        ADV(p);
        if (!AT_END(p) && PEEK(p) == '\'') {
            multiline = true;
            ADV(p);
            if (!AT_END(p) && PEEK(p) == '\n') {
                ADV(p);
                p->line++;
                p->col = 1;
            } else if (!AT_END(p) && PEEK(p) == '\r') {
                ADV(p);
                if (!AT_END(p) && PEEK(p) == '\n')
                    ADV(p);
                p->line++;
                p->col = 1;
            }
        } else {
            return new_string(p, toml_duplicate(p, ""), 0);
        }
    }

    const char *start = p->data + p->pos;
    size_t slen = 0;

    while (!AT_END(p)) {
        if (multiline) {
            if (PEEK(p) == '\'' && p->pos + 2 < p->len && p->data[p->pos + 1] == '\'' &&
                p->data[p->pos + 2] == '\'') {
                p->pos += 3;
                p->col += 3;
                for (unsigned quotes = 0; quotes < 2 && !AT_END(p) && PEEK(p) == '\''; ++quotes) {
                    ++slen;
                    ADV(p);
                }
                goto lit_done;
            }
        } else {
            if (PEEK(p) == '\'') {
                ADV(p);
                goto lit_done;
            }
            if (PEEK(p) == '\n' || PEEK(p) == '\r') {
                p->error = true;
                return NULL;
            }
        }
        if (PEEK(p) == '\n') {
            p->line++;
            p->col = 1;
        }
        ADV(p);
        slen++;
    }
    p->error = true;
    return NULL;

lit_done: {
    XrTomlValue *v = alloc_value(p, XR_TOML_STRING);
    if (!v)
        return NULL;
    v->as.string = (char *) toml_allocate(p, slen + 1, false);
    if (!v->as.string) {
        xr_free(v);
        return NULL;
    }
    memcpy(v->as.string, start, slen);
    v->as.string[slen] = '\0';
    v->string_length = slen;
    return v;
}
}

/* ========== Number Parsing ========== */

static unsigned number_digit(char c) {
    if (c >= '0' && c <= '9') return (unsigned) (c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned) (c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (unsigned) (c - 'A' + 10);
    return 16;
}

static bool number_digits(const char *text, size_t length, size_t *pos, unsigned base) {
    if (*pos == length || number_digit(text[*pos]) >= base) return false;
    do {
        ++*pos;
        if (*pos < length && text[*pos] == '_') {
            ++*pos;
            if (*pos == length || number_digit(text[*pos]) >= base) return false;
        }
    } while (*pos < length && number_digit(text[*pos]) < base);
    return true;
}

/* Check grammar before removing separators or invoking numeric conversion. */
static bool valid_number(const char *text, size_t length) {
    size_t pos = 0;
    if (length && (text[0] == '+' || text[0] == '-')) ++pos;
    if (pos == length) return false;
    if (!pos && length > 2 && text[0] == '0') {
        unsigned base = text[1] == 'x' ? 16 : text[1] == 'o' ? 8 : text[1] == 'b' ? 2 : 0;
        if (base) {
            pos = 2;
            return number_digits(text, length, &pos, base) && pos == length;
        }
    }
    size_t first = pos;
    if (!number_digits(text, length, &pos, 10)) return false;
    if (text[first] == '0' && pos - first > 1) return false;
    if (pos < length && text[pos] == '.') {
        ++pos;
        if (!number_digits(text, length, &pos, 10)) return false;
    }
    if (pos < length && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        if (pos < length && (text[pos] == '+' || text[pos] == '-')) ++pos;
        if (!number_digits(text, length, &pos, 10)) return false;
    }
    return pos == length;
}

static XrTomlValue *parse_number(TomlCtx *p) {
    const char *start = p->data + p->pos;
    bool is_float = false;
    bool is_hex = false, is_oct = false, is_bin = false;
    bool negative = false;

    if (PEEK(p) == '+' || PEEK(p) == '-') {
        negative = (PEEK(p) == '-');
        ADV(p);
    }

    /* inf / nan */
    if (p->pos + 3 <= p->len) {
        if (strncmp(p->data + p->pos, "inf", 3) == 0 &&
            (p->pos + 3 >= p->len || !isalnum((unsigned char) p->data[p->pos + 3]))) {
            p->pos += 3;
            p->col += 3;
            XrTomlValue *v = alloc_value(p, XR_TOML_FLOAT);
            if (v)
                v->as.number = negative ? -INFINITY : INFINITY;
            return v;
        }
        if (strncmp(p->data + p->pos, "nan", 3) == 0 &&
            (p->pos + 3 >= p->len || !isalnum((unsigned char) p->data[p->pos + 3]))) {
            p->pos += 3;
            p->col += 3;
            XrTomlValue *v = alloc_value(p, XR_TOML_FLOAT);
            if (v)
                v->as.number = NAN;
            return v;
        }
    }

    /* Prefix: 0x, 0o, 0b */
    if (p->pos + 1 < p->len && p->data[p->pos] == '0') {
        char nx = p->data[p->pos + 1];
        if (nx == 'x') {
            is_hex = true;
            p->pos += 2;
            p->col += 2;
        } else if (nx == 'o') {
            is_oct = true;
            p->pos += 2;
            p->col += 2;
        } else if (nx == 'b') {
            is_bin = true;
            p->pos += 2;
            p->col += 2;
        }
    }

    /* Consume digits (with underscores) */
    while (!AT_END(p)) {
        char c = PEEK(p);
        if (c == '_') {
            ADV(p);
            continue;
        }
        if (is_hex) {
            if (!isxdigit((unsigned char) c))
                break;
        } else if (is_oct) {
            if (c < '0' || c > '7')
                break;
        } else if (is_bin) {
            if (c != '0' && c != '1')
                break;
        } else {
            if (!isdigit((unsigned char) c) && c != '.' && c != 'e' && c != 'E' && c != '+' &&
                c != '-')
                break;
            if (c == '.' || c == 'e' || c == 'E')
                is_float = true;
        }
        ADV(p);
    }

    /* Strip underscores into temp buf */
    size_t raw_len = (size_t) ((p->data + p->pos) - start);
    if (!valid_number(start, raw_len)) { p->error = true; return NULL; }
    buf_reset(p);
    for (size_t i = 0; i < raw_len; i++) {
        if (start[i] != '_')
            buf_char(p, start[i]);
    }
    buf_char(p, '\0');
    if (p->error)
        return NULL;
    char *num = p->buf;
    (void) p->buf_len; /* num is NUL-terminated in buf */

    if (is_hex || is_oct || is_bin) {
        int base = is_hex ? 16 : (is_oct ? 8 : 2);
        /* Nondecimal integers have no sign in TOML. */
        const char *digits = num + 2;
        errno = 0;
        int64_t val = strtoll(digits, NULL, base);
        if (errno == ERANGE) {
            p->error = true;
            return NULL;
        }
        XrTomlValue *v = alloc_value(p, XR_TOML_INTEGER);
        if (v)
            v->as.integer = val;
        return v;
    }

    if (is_float) {
        char *end = NULL;
        double number = strtod(num, &end);
        if (!end || *end) { p->error = true; return NULL; }
        XrTomlValue *v = alloc_value(p, XR_TOML_FLOAT);
        if (v)
            v->as.number = number;
        return v;
    }

    /* Decimal integer */
    errno = 0;
    int64_t ival = strtoll(num, NULL, 10);
    if (errno == ERANGE) {
        p->error = true;
        return NULL;
    }
    XrTomlValue *v = alloc_value(p, XR_TOML_INTEGER);
    if (v)
        v->as.integer = ival;
    return v;
}

/* ========== Datetime Parsing ========== */

static int date_digits(const char *text, size_t count) {
    int value = 0;
    for (size_t i = 0; i < count; ++i) {
        if (text[i] < '0' || text[i] > '9') return -1;
        value = value * 10 + text[i] - '0';
    }
    return value;
}

static bool valid_date(const char *text) {
    int year = date_digits(text, 4), month = date_digits(text + 5, 2), day = date_digits(text + 8, 2);
    if (year < 0 || month < 1 || month > 12 || day < 1 || text[4] != '-' || text[7] != '-')
        return false;
    const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day <= days[month - 1] + (month == 2 && leap);
}

static bool valid_datetime(const char *text, size_t length) {
    size_t pos = 0;
    bool dated = length >= 10 && text[4] == '-';
    if (dated) {
        if (!valid_date(text)) return false;
        pos = 10;
        if (pos == length) return true;
        if (text[pos] != 'T' && text[pos] != 't' && text[pos] != ' ') return false;
        ++pos;
    }
    if (length - pos < 8 || text[pos + 2] != ':' || text[pos + 5] != ':') return false;
    int hour = date_digits(text + pos, 2), minute = date_digits(text + pos + 3, 2);
    int second = date_digits(text + pos + 6, 2);
    if (hour < 0 || hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60)
        return false;
    pos += 8;
    if (pos < length && text[pos] == '.') {
        size_t first = ++pos;
        while (pos < length && text[pos] >= '0' && text[pos] <= '9') ++pos;
        if (pos == first) return false;
    }
    if (pos == length) return true;
    if (!dated) return false;
    if (text[pos] == 'Z' || text[pos] == 'z') return pos + 1 == length;
    if (length - pos != 6 || (text[pos] != '+' && text[pos] != '-') || text[pos + 3] != ':')
        return false;
    hour = date_digits(text + pos + 1, 2);
    minute = date_digits(text + pos + 4, 2);
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

/* TOML datetimes are stored as strings at this layer. */
static XrTomlValue *parse_datetime(TomlCtx *p) {
    const char *start = p->data + p->pos;
    bool found_t = false;

    while (!AT_END(p)) {
        char c = PEEK(p);
        if (isdigit((unsigned char) c) || c == '-' || c == ':' || c == 'T' || c == 't' ||
            c == 'Z' || c == 'z' || c == '+' || c == '.') {
            if (c == 'T' || c == 't')
                found_t = true;
            ADV(p);
        } else if (c == ' ' && !found_t) {
            /* TOML v1.0: space can replace T */
            size_t ahead = p->pos + 1;
            if (ahead < p->len && isdigit((unsigned char) p->data[ahead])) {
                found_t = true;
                ADV(p);
            } else {
                break;
            }
        } else {
            break;
        }
    }

    size_t slen = (size_t) ((p->data + p->pos) - start);
    if (!valid_datetime(start, slen)) { p->error = true; return NULL; }
    XrTomlValue *v = alloc_value(p, XR_TOML_DATETIME);
    if (!v)
        return NULL;
    v->as.string = (char *) toml_allocate(p, slen + 1, false);
    if (!v->as.string) {
        xr_free(v);
        return NULL;
    }
    memcpy(v->as.string, start, slen);
    v->as.string[slen] = '\0';
    v->string_length = slen;
    return v;
}

/* ========== Array Parsing ========== */

static XrTomlValue *parse_array_value(TomlCtx *p) {
    if (PEEK(p) != '[') {
        p->error = true;
        return NULL;
    }
    ADV(p);

    XrTomlValue *arr = new_array(p);
    if (!arr)
        return NULL;

    skip_ws_nl(p);

    if (!AT_END(p) && PEEK(p) == ']') {
        ADV(p);
        return arr;
    }

    while (!AT_END(p) && !p->error) {
        skip_ws_nl(p);
        XrTomlValue *val = parse_value(p, arr->depth + 1);
        if (!val) {
            xtoml_free(arr);
            return NULL;
        }
        if (!array_push(p, arr, val)) {
            xtoml_free(val);
            xtoml_free(arr);
            p->error = true;
            return NULL;
        }

        skip_ws_nl(p);
        if (AT_END(p))
            break;
        if (PEEK(p) == ']') {
            ADV(p);
            return arr;
        }
        if (PEEK(p) == ',') {
            ADV(p);
            /* TOML permits a trailing comma in arrays.  Consume it here
             * instead of attempting to parse the closing bracket as a value. */
            skip_ws_nl(p);
            if (!AT_END(p) && PEEK(p) == ']') {
                ADV(p);
                return arr;
            }
            continue;
        }
        p->error = true;
        xtoml_free(arr);
        return NULL;
    }

    /* Unterminated array */
    xtoml_free(arr);
    p->error = true;
    return NULL;
}

/* ========== Inline Table Parsing ========== */

/* Forward: parse a single key segment (bare, basic-quoted, or literal-quoted).
 * Returns xr_strdup'd string, caller frees. */
static char *parse_key_seg(TomlCtx *p, size_t *length);
/* Forward: parse dotted key path. Returns array of strdup'd keys. */
static TomlKey *parse_key_path(TomlCtx *p, int *nkeys);
static void free_keys(TomlKey *keys, int n);

static XrTomlValue *parse_inline_table(TomlCtx *p) {
    if (PEEK(p) != '{') {
        p->error = true;
        return NULL;
    }
    ADV(p);

    XrTomlValue *tbl = new_table(p);
    if (!tbl)
        return NULL;

    skip_ws(p);
    if (!AT_END(p) && PEEK(p) == '}') {
        ADV(p);
        tbl->as.table.origin = XR_TOML_TABLE_INLINE;
        return tbl;
    }

    while (!AT_END(p) && !p->error) {
        skip_ws(p);
        int nkeys = 0;
        TomlKey *keys = parse_key_path(p, &nkeys);
        if (!keys || nkeys == 0) {
            p->error = true;
            xtoml_free(tbl);
            return NULL;
        }

        skip_ws(p);
        if (AT_END(p) || PEEK(p) != '=') {
            free_keys(keys, nkeys);
            p->error = true;
            xtoml_free(tbl);
            return NULL;
        }
        ADV(p);
        skip_ws(p);

        XrTomlValue *val = parse_value(p, tbl->depth + (uint32_t) nkeys);
        if (!val) {
            free_keys(keys, nkeys);
            xtoml_free(tbl);
            return NULL;
        }
        set_nested(p, tbl, keys, nkeys, val);
        free_keys(keys, nkeys);
        if (p->error) {
            xtoml_free(tbl);
            return NULL;
        }

        skip_ws(p);
        if (AT_END(p))
            break;
        if (PEEK(p) == '}') {
            ADV(p);
            tbl->as.table.origin = XR_TOML_TABLE_INLINE;
            return tbl;
        }
        if (PEEK(p) == ',') {
            ADV(p);
            continue;
        }
        p->error = true;
        xtoml_free(tbl);
        return NULL;
    }

    xtoml_free(tbl);
    p->error = true;
    return NULL;
}

/* ========== Value Dispatch ========== */

static XrTomlValue *parse_value_body(TomlCtx *p) {
    skip_ws(p);
    if (AT_END(p)) {
        p->error = true;
        return NULL;
    }

    char c = PEEK(p);

    if (c == '"')
        return parse_basic_string(p);
    if (c == '\'')
        return parse_literal_string(p);

    /* true / false */
    if (p->pos + 4 <= p->len && strncmp(p->data + p->pos, "true", 4) == 0 &&
        (p->pos + 4 >= p->len || !isalnum((unsigned char) p->data[p->pos + 4]))) {
        p->pos += 4;
        p->col += 4;
        XrTomlValue *v = alloc_value(p, XR_TOML_BOOL);
        if (v)
            v->as.boolean = true;
        return v;
    }
    if (p->pos + 5 <= p->len && strncmp(p->data + p->pos, "false", 5) == 0 &&
        (p->pos + 5 >= p->len || !isalnum((unsigned char) p->data[p->pos + 5]))) {
        p->pos += 5;
        p->col += 5;
        XrTomlValue *v = alloc_value(p, XR_TOML_BOOL);
        if (v)
            v->as.boolean = false;
        return v;
    }

    if (c == '[')
        return parse_array_value(p);
    if (c == '{')
        return parse_inline_table(p);

    /* Bare inf/nan */
    if (p->pos + 3 <= p->len) {
        if (strncmp(p->data + p->pos, "inf", 3) == 0 &&
            (p->pos + 3 >= p->len || !isalnum((unsigned char) p->data[p->pos + 3]))) {
            p->pos += 3;
            p->col += 3;
            XrTomlValue *v = alloc_value(p, XR_TOML_FLOAT);
            if (v)
                v->as.number = INFINITY;
            return v;
        }
        if (strncmp(p->data + p->pos, "nan", 3) == 0 &&
            (p->pos + 3 >= p->len || !isalnum((unsigned char) p->data[p->pos + 3]))) {
            p->pos += 3;
            p->col += 3;
            XrTomlValue *v = alloc_value(p, XR_TOML_FLOAT);
            if (v)
                v->as.number = NAN;
            return v;
        }
    }

    /* Date and local-time prefixes select the same validated textual representation. */
    if (c >= '0' && c <= '9' && p->len - p->pos >= 3 && p->data[p->pos + 2] == ':')
        return parse_datetime(p);
    if (isdigit((unsigned char) c) && p->pos + 10 <= p->len) {
        const char *d = p->data + p->pos;
        if (isdigit(d[0]) && isdigit(d[1]) && isdigit(d[2]) && isdigit(d[3]) && d[4] == '-') {
            return parse_datetime(p);
        }
    }

    /* Number */
    if (isdigit((unsigned char) c) || c == '+' || c == '-') {
        return parse_number(p);
    }

    p->error = true;
    return NULL;
}

static XrTomlValue *parse_value(TomlCtx *p, uint32_t depth) {
    if (depth > p->depth_limit) { parse_limit(p); return NULL; }
    uint32_t saved = p->depth;
    p->depth = depth;
    XrTomlValue *value = parse_value_body(p);
    p->depth = saved;
    return value;
}

/* ========== Key Parsing ========== */

static char *parse_key_seg(TomlCtx *p, size_t *length) {
    if (p->len - p->pos >= 3 && (PEEK(p) == '\"' || PEEK(p) == '\'') &&
        p->data[p->pos + 1] == PEEK(p) && p->data[p->pos + 2] == PEEK(p)) {
        p->error = true;
        return NULL;
    }
    if (PEEK(p) == '"') {
        XrTomlValue *sv = parse_basic_string(p);
        if (!sv)
            return NULL;
        *length = sv->string_length;
        char *k = sv->as.string;
        sv->as.string = NULL;
        xr_free(sv);
        return k;
    }
    if (PEEK(p) == '\'') {
        XrTomlValue *sv = parse_literal_string(p);
        if (!sv)
            return NULL;
        *length = sv->string_length;
        char *k = sv->as.string;
        sv->as.string = NULL;
        xr_free(sv);
        return k;
    }

    /* Bare key */
    const char *start = p->data + p->pos;
    size_t klen = 0;
    while (!AT_END(p) && is_bare_key_char(PEEK(p))) {
        ADV(p);
        klen++;
    }
    if (klen == 0) {
        p->error = true;
        return NULL;
    }
    char *k = (char *) toml_allocate(p, klen + 1, false);
    if (!k)
        return NULL;
    memcpy(k, start, klen);
    k[klen] = '\0';
    *length = klen;
    return k;
}

static void free_keys(TomlKey *keys, int n) {
    if (!keys)
        return;
    for (int i = 0; i < n; i++)
        xr_free(keys[i].text);
    xr_free(keys);
}

static TomlKey *parse_key_path(TomlCtx *p, int *nkeys) {
    *nkeys = 0;
    int cap = 4;
    TomlKey *keys = (TomlKey *) toml_allocate(p, (size_t) cap * sizeof(TomlKey), false);
    if (!keys)
        return NULL;

    while (!AT_END(p)) {
        if ((uint32_t) *nkeys >= p->depth_limit) {
            parse_limit(p); free_keys(keys, *nkeys); return NULL;
        }
        skip_ws(p);
        size_t length = 0;
        char *seg = parse_key_seg(p, &length);
        if (!seg) {
            free_keys(keys, *nkeys);
            return NULL;
        }

        if (*nkeys >= cap) {
            cap = grown_capacity(p, cap, sizeof(TomlKey));
            if (!cap) {
                xr_free(seg); free_keys(keys, *nkeys); return NULL;
            }
            TomlKey *tmp = (TomlKey *) toml_resize(p, keys, (size_t) cap * sizeof(TomlKey));
            if (!tmp) {
                xr_free(seg);
                free_keys(keys, *nkeys);
                return NULL;
            }
            keys = tmp;
        }
        keys[(*nkeys)++] = (TomlKey){seg, length};

        skip_ws(p);
        if (AT_END(p) || PEEK(p) != '.')
            break;
        ADV(p); /* skip '.' */
    }
    return keys;
}

/* ========== Nested Value Setting ========== */

/* Consume val on every path; an unsuccessful assignment invalidates the document. */
static void set_nested(TomlCtx *p, XrTomlValue *root, TomlKey *keys, int nkeys, XrTomlValue *val) {
    XrTomlValue *cur = root;
    for (int i = 0; i < nkeys - 1; i++) {
        int idx = table_find(p, cur, keys[i].text, keys[i].length);
        if (idx >= 0) {
            XrTomlValue *existing = cur->as.table.members[idx].value;
            if (existing->type != XR_TOML_TABLE ||
                existing->as.table.origin == XR_TOML_TABLE_INLINE ||
                existing->as.table.origin == XR_TOML_TABLE_HEADER)
                goto fail;
            existing->as.table.origin = XR_TOML_TABLE_DOTTED;
            cur = existing;
        } else {
            p->depth = cur->depth + 1;
            XrTomlValue *nt = new_table(p);
            if (!nt)
                goto fail;
            nt->as.table.origin = XR_TOML_TABLE_DOTTED;
            if (!table_set(p, cur, keys[i].text, keys[i].length, nt)) {
                xtoml_free(nt);
                goto fail;
            }
            cur = nt;
        }
    }
    if (nkeys > 0 && table_set(p, cur, keys[nkeys - 1].text, keys[nkeys - 1].length, val))
        return;
fail:
    xtoml_free(val);
    p->error = true;
}

/* ========== Table Header Navigation ========== */

/* Navigate/create path in root, returning the leaf table.
 * Used for [table.path] headers. */
static XrTomlValue *get_or_create_table(TomlCtx *p, XrTomlValue *root, TomlKey *keys, int nkeys) {
    XrTomlValue *cur = root;
    for (int i = 0; i < nkeys; i++) {
        int idx = table_find(p, cur, keys[i].text, keys[i].length);
        if (idx >= 0) {
            XrTomlValue *existing = cur->as.table.members[idx].value;
            if (existing->type == XR_TOML_TABLE) {
                if (existing->as.table.origin == XR_TOML_TABLE_INLINE ||
                    (i == nkeys - 1 && existing->as.table.origin != XR_TOML_TABLE_IMPLICIT))
                    return NULL;
                cur = existing;
            } else if (existing->type == XR_TOML_ARRAY && existing->as.array.table_sequence &&
                       existing->as.array.count > 0 && i < nkeys - 1) {
                /* For array-of-tables intermediate: use last element */
                XrTomlValue *last = existing->as.array.items[existing->as.array.count - 1];
                if (last->type == XR_TOML_TABLE) {
                    cur = last;
                } else {
                    return NULL;
                }
            } else {
                return NULL;
            }
        } else {
            p->depth = cur->depth + 1;
            XrTomlValue *nt = new_table(p);
            if (!nt)
                return NULL;
            if (!table_set(p, cur, keys[i].text, keys[i].length, nt)) {
                xtoml_free(nt);
                return NULL;
            }
            cur = nt;
        }
    }
    cur->as.table.origin = XR_TOML_TABLE_HEADER;
    return cur;
}

/* Navigate/create path for [[array.table]] headers.
 * Creates a new table, appends it to the array at the last key. */
static XrTomlValue *get_or_create_array_table(TomlCtx *p, XrTomlValue *root, TomlKey *keys, int nkeys) {
    XrTomlValue *cur = root;

    /* Navigate intermediate path */
    for (int i = 0; i < nkeys - 1; i++) {
        int idx = table_find(p, cur, keys[i].text, keys[i].length);
        if (idx >= 0) {
            XrTomlValue *existing = cur->as.table.members[idx].value;
            if (existing->type == XR_TOML_TABLE) {
                if (existing->as.table.origin == XR_TOML_TABLE_INLINE)
                    return NULL;
                cur = existing;
            } else if (existing->type == XR_TOML_ARRAY && existing->as.array.table_sequence &&
                       existing->as.array.count > 0) {
                XrTomlValue *last = existing->as.array.items[existing->as.array.count - 1];
                if (last->type == XR_TOML_TABLE)
                    cur = last;
                else
                    return NULL;
            } else {
                return NULL;
            }
        } else {
            p->depth = cur->depth + 1;
            XrTomlValue *nt = new_table(p);
            if (!nt)
                return NULL;
            if (!table_set(p, cur, keys[i].text, keys[i].length, nt)) {
                xtoml_free(nt);
                return NULL;
            }
            cur = nt;
        }
    }

    if (nkeys <= 0)
        return cur;

    /* Last key: get or create array, append new table */
    TomlKey last_key = keys[nkeys - 1];
    int idx = table_find(p, cur, last_key.text, last_key.length);
    XrTomlValue *arr;
    if (idx >= 0) {
        arr = cur->as.table.members[idx].value;
        if (arr->type != XR_TOML_ARRAY || !arr->as.array.table_sequence)
            return NULL;
    } else {
        p->depth = cur->depth + 1;
        arr = new_array(p);
        if (!arr)
            return NULL;
        arr->as.array.table_sequence = true;
        if (!table_set(p, cur, last_key.text, last_key.length, arr)) {
            xtoml_free(arr);
            return NULL;
        }
    }

    p->depth = arr->depth + 1;
    XrTomlValue *nt = new_table(p);
    if (!nt)
        return NULL;
    nt->as.table.origin = XR_TOML_TABLE_HEADER;
    if (!array_push(p, arr, nt)) {
        xtoml_free(nt);
        return NULL;
    }
    return nt;
}

/* ========== Main Parse Function ========== */

/* Validate raw text before decoding escapes; escaped control scalars remain valid. */
static bool valid_document_text(const char *data, size_t len) {
    if (xr_utf8_core_scan_strict((const uint8_t *) data, len).error != XR_UTF8_OK)
        return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char) data[i];
        if (c == '\r') {
            if (i + 1 == len || data[i + 1] != '\n') return false;
        } else if ((c < 0x20 && c != '\t' && c != '\n') || c == 0x7F) {
            return false;
        }
    }
    return true;
}

static XrTomlValue *parse_document(TomlCtx *p, const char *data, size_t len) {
    if (!data || !valid_document_text(data, len))
        return NULL;

    XrTomlValue *root = new_table(p);
    if (!root) {
        ctx_cleanup(p);
        return NULL;
    }

    XrTomlValue *current_table = root;

    while (!AT_END(p) && !p->error) {
        skip_ws_nl(p);
        if (AT_END(p))
            break;

        if (PEEK(p) == '[') {
            ADV(p);

            bool is_array_table = false;
            if (!AT_END(p) && PEEK(p) == '[') {
                is_array_table = true;
                ADV(p);
            }

            skip_ws(p);

            int nkeys = 0;
            TomlKey *keys = parse_key_path(p, &nkeys);
            if (!keys || nkeys == 0) {
                p->error = true;
                free_keys(keys, nkeys);
                break;
            }

            skip_ws(p);

            if (AT_END(p) || PEEK(p) != ']') {
                p->error = true;
                free_keys(keys, nkeys);
                break;
            }
            ADV(p);

            if (is_array_table) {
                if (AT_END(p) || PEEK(p) != ']') {
                    p->error = true;
                    free_keys(keys, nkeys);
                    break;
                }
                ADV(p);
                current_table = get_or_create_array_table(p, root, keys, nkeys);
            } else {
                current_table = get_or_create_table(p, root, keys, nkeys);
            }

            free_keys(keys, nkeys);

            if (!current_table) {
                p->error = true;
                break;
            }

            finish_line(p);
            continue;
        }

        /* Key = Value */
        int nkeys = 0;
        TomlKey *keys = parse_key_path(p, &nkeys);
        if (!keys || nkeys == 0) {
            p->error = true;
            free_keys(keys, nkeys);
            break;
        }

        skip_ws(p);

        if (AT_END(p) || PEEK(p) != '=') {
            p->error = true;
            free_keys(keys, nkeys);
            break;
        }
        ADV(p);

        skip_ws(p);

        XrTomlValue *val = parse_value(p, current_table->depth + (uint32_t) nkeys);
        if (!val) {
            free_keys(keys, nkeys);
            p->error = true;
            break;
        }

        set_nested(p, current_table, keys, nkeys, val);
        free_keys(keys, nkeys);

        finish_line(p);
    }

    ctx_cleanup(p);

    if (p->error) {
        xtoml_free(root);
        return NULL;
    }
    return root;
}

XR_FUNC XrTomlValue *xtoml_parse_limited(const char *data, size_t len,
    XrTomlParseBudget budget, XrTomlParseStatus *status, size_t *work_used) {
    if (work_used) *work_used = 0;
    if (status) *status = XR_TOML_PARSE_INVALID;
    if (!data) return NULL;
    if (len > budget.input_bytes || len > budget.work) {
        if (status) *status = XR_TOML_PARSE_LIMIT;
        return NULL;
    }
    TomlCtx context;
    ctx_init(&context, data, len);
    context.allocation_remaining = budget.allocation_bytes;
    context.work_remaining = budget.work - len;
    context.depth_limit = budget.depth < 128 ? budget.depth : 128;
    context.status = XR_TOML_PARSE_INVALID;
    XrTomlValue *root = parse_document(&context, data, len);
    if (work_used) *work_used = budget.work - context.work_remaining;
    if (status) *status = root ? XR_TOML_PARSE_OK : context.status;
    return root;
}

XR_FUNC XrTomlValue *xtoml_parse(const char *data, size_t len) {
    XrTomlParseBudget budget = {16u * 1024u * 1024u, 64u * 1024u * 1024u, 64u * 1024u * 1024u, 128};
    return xtoml_parse_limited(data, len, budget, NULL, NULL);
}

/* ========== Accessors ========== */

XR_FUNC XrTomlValue *xtoml_get(XrTomlValue *table, const char *key) {
    if (!table || table->type != XR_TOML_TABLE || !key)
        return NULL;
    int idx = table_find(NULL, table, key, strlen(key));
    return idx >= 0 ? table->as.table.members[idx].value : NULL;
}

XR_FUNC const char *xtoml_get_string(XrTomlValue *table, const char *key) {
    XrTomlValue *v = xtoml_get(table, key);
    return (v && v->type == XR_TOML_STRING) ? v->as.string : NULL;
}

XR_FUNC int64_t xtoml_get_int(XrTomlValue *table, const char *key) {
    XrTomlValue *v = xtoml_get(table, key);
    return (v && v->type == XR_TOML_INTEGER) ? v->as.integer : 0;
}

XR_FUNC int64_t xtoml_get_int_or(XrTomlValue *table, const char *key, int64_t default_val) {
    XrTomlValue *v = xtoml_get(table, key);
    return (v && v->type == XR_TOML_INTEGER) ? v->as.integer : default_val;
}

XR_FUNC double xtoml_get_float(XrTomlValue *table, const char *key) {
    XrTomlValue *v = xtoml_get(table, key);
    return (v && v->type == XR_TOML_FLOAT) ? v->as.number : 0.0;
}

XR_FUNC bool xtoml_get_bool(XrTomlValue *table, const char *key) {
    XrTomlValue *v = xtoml_get(table, key);
    return (v && v->type == XR_TOML_BOOL) ? v->as.boolean : false;
}

XR_FUNC bool xtoml_get_bool_or(XrTomlValue *table, const char *key, bool default_val) {
    XrTomlValue *v = xtoml_get(table, key);
    return (v && v->type == XR_TOML_BOOL) ? v->as.boolean : default_val;
}

XR_FUNC XrTomlValue *xtoml_get_table(XrTomlValue *table, const char *key) {
    XrTomlValue *v = xtoml_get(table, key);
    return (v && v->type == XR_TOML_TABLE) ? v : NULL;
}

XR_FUNC XrTomlValue *xtoml_get_array(XrTomlValue *table, const char *key) {
    XrTomlValue *v = xtoml_get(table, key);
    return (v && v->type == XR_TOML_ARRAY) ? v : NULL;
}

XR_FUNC int xtoml_array_len(XrTomlValue *arr) {
    return (arr && arr->type == XR_TOML_ARRAY) ? arr->as.array.count : 0;
}

XR_FUNC XrTomlValue *xtoml_array_get(XrTomlValue *arr, int index) {
    if (!arr || arr->type != XR_TOML_ARRAY)
        return NULL;
    if (index < 0 || index >= arr->as.array.count)
        return NULL;
    return arr->as.array.items[index];
}

XR_FUNC int xtoml_table_count(XrTomlValue *table) {
    return (table && table->type == XR_TOML_TABLE) ? table->as.table.count : 0;
}

/* ========== Type Checks ========== */

XR_FUNC bool xtoml_is_string(XrTomlValue *v) {
    return v && v->type == XR_TOML_STRING;
}
XR_FUNC bool xtoml_is_integer(XrTomlValue *v) {
    return v && v->type == XR_TOML_INTEGER;
}
XR_FUNC bool xtoml_is_float(XrTomlValue *v) {
    return v && v->type == XR_TOML_FLOAT;
}
XR_FUNC bool xtoml_is_bool(XrTomlValue *v) {
    return v && v->type == XR_TOML_BOOL;
}
XR_FUNC bool xtoml_is_datetime(XrTomlValue *v) {
    return v && v->type == XR_TOML_DATETIME;
}
XR_FUNC bool xtoml_is_array(XrTomlValue *v) {
    return v && v->type == XR_TOML_ARRAY;
}
XR_FUNC bool xtoml_is_table(XrTomlValue *v) {
    return v && v->type == XR_TOML_TABLE;
}
