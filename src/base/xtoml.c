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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <inttypes.h>

/* ========== Allocators ========== */

static XrTomlValue *alloc_value(XrTomlType type) {
    XrTomlValue *v = (XrTomlValue *) xr_calloc(1, sizeof(XrTomlValue));
    if (v)
        v->type = type;
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

/* ========== Table Helpers ========== */

static XrTomlValue *new_table(void) {
    XrTomlValue *t = alloc_value(XR_TOML_TABLE);
    if (!t)
        return NULL;
    t->as.table.capacity = 8;
    t->as.table.members =
        (XrTomlMember *) xr_calloc((size_t) t->as.table.capacity, sizeof(XrTomlMember));
    if (!t->as.table.members) {
        xr_free(t);
        return NULL;
    }
    return t;
}

/* Find member by key. Returns index or -1. */
static int table_find(XrTomlValue *t, const char *key) {
    if (!t || t->type != XR_TOML_TABLE)
        return -1;
    for (int i = 0; i < t->as.table.count; i++) {
        if (strcmp(t->as.table.members[i].key, key) == 0)
            return i;
    }
    return -1;
}

/* Set key=value in table. If key exists, replaces value. */
static bool table_set(XrTomlValue *t, const char *key, XrTomlValue *val) {
    if (!t || t->type != XR_TOML_TABLE || !key)
        return false;

    int idx = table_find(t, key);
    if (idx >= 0) {
        xtoml_free(t->as.table.members[idx].value);
        t->as.table.members[idx].value = val;
        return true;
    }

    if (t->as.table.count >= t->as.table.capacity) {
        int new_cap = t->as.table.capacity * 2;
        XrTomlMember *tmp = (XrTomlMember *) xr_realloc(t->as.table.members,
                                                        (size_t) new_cap * sizeof(XrTomlMember));
        if (!tmp)
            return false;
        t->as.table.members = tmp;
        t->as.table.capacity = new_cap;
    }
    t->as.table.members[t->as.table.count].key = xr_strdup(key);
    t->as.table.members[t->as.table.count].value = val;
    t->as.table.count++;
    return true;
}

/* ========== Array Helpers ========== */

static XrTomlValue *new_array(void) {
    XrTomlValue *a = alloc_value(XR_TOML_ARRAY);
    if (!a)
        return NULL;
    a->as.array.capacity = 8;
    a->as.array.items =
        (XrTomlValue **) xr_calloc((size_t) a->as.array.capacity, sizeof(XrTomlValue *));
    if (!a->as.array.items) {
        xr_free(a);
        return NULL;
    }
    return a;
}

static bool array_push(XrTomlValue *a, XrTomlValue *val) {
    if (!a || a->type != XR_TOML_ARRAY)
        return false;
    if (a->as.array.count >= a->as.array.capacity) {
        int new_cap = a->as.array.capacity * 2;
        XrTomlValue **tmp = (XrTomlValue **) xr_realloc(a->as.array.items,
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

typedef struct {
    const char *data;
    size_t len;
    size_t pos;
    int line;
    int col;
    bool error;

    /* Reusable temp buffer for strings with escapes */
    char *buf;
    size_t buf_len;
    size_t buf_cap;
} TomlCtx;

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
    size_t new_cap = p->buf_cap ? p->buf_cap * 2 : 64;
    while (new_cap < needed)
        new_cap *= 2;
    char *tmp = (char *) xr_realloc(p->buf, new_cap);
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
    buf_ensure(p, p->buf_len + 2);
    if (p->buf)
        p->buf[p->buf_len++] = c;
}

static char *buf_dup(TomlCtx *p) {
    buf_ensure(p, p->buf_len + 1);
    if (!p->buf)
        return xr_strdup("");
    p->buf[p->buf_len] = '\0';
    return xr_strdup(p->buf);
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

static void skip_to_eol(TomlCtx *p) {
    while (!AT_END(p) && PEEK(p) != '\n' && PEEK(p) != '\r')
        ADV(p);
}

static bool is_bare_key_char(char c) {
    return isalnum((unsigned char) c) || c == '_' || c == '-';
}

/* ========== Forward Declarations ========== */

static XrTomlValue *parse_value(TomlCtx *p);
static void set_nested(TomlCtx *p, XrTomlValue *root, char **keys, int nkeys, XrTomlValue *val);

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
            XrTomlValue *v = alloc_value(XR_TOML_STRING);
            if (v)
                v->as.string = xr_strdup("");
            return v;
        }
    }

    buf_reset(p);

    while (!AT_END(p)) {
        if (multiline) {
            if (PEEK(p) == '"' && p->pos + 2 < p->len && p->data[p->pos + 1] == '"' &&
                p->data[p->pos + 2] == '"') {
                p->pos += 3;
                p->col += 3;
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
            if (AT_END(p))
                break;
            char c = PEEK(p);
            ADV(p);
            switch (c) {
                case 'n':
                    buf_char(p, '\n');
                    break;
                case 't':
                    buf_char(p, '\t');
                    break;
                case 'r':
                    buf_char(p, '\r');
                    break;
                case '\\':
                    buf_char(p, '\\');
                    break;
                case '"':
                    buf_char(p, '"');
                    break;
                case 'b':
                    buf_char(p, '\b');
                    break;
                case 'f':
                    buf_char(p, '\f');
                    break;
                case 'u':
                case 'U': {
                    int digits = (c == 'u') ? 4 : 8;
                    unsigned int cp = 0;
                    for (int i = 0; i < digits && !AT_END(p); i++) {
                        char h = PEEK(p);
                        if (!isxdigit((unsigned char) h)) {
                            p->error = true;
                            return NULL;
                        }
                        ADV(p);
                        cp <<= 4;
                        if (h >= '0' && h <= '9')
                            cp |= (unsigned) (h - '0');
                        else if (h >= 'a' && h <= 'f')
                            cp |= (unsigned) (h - 'a' + 10);
                        else
                            cp |= (unsigned) (h - 'A' + 10);
                    }
                    if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
                        p->error = true;
                        return NULL;
                    }
                    utf8_encode(p, cp);
                    break;
                }
                case '\n':
                case '\r':
                    if (multiline) {
                        if (c == '\r' && !AT_END(p) && PEEK(p) == '\n')
                            ADV(p);
                        p->line++;
                        p->col = 1;
                        /* Trim whitespace continuation */
                        while (!AT_END(p) && (PEEK(p) == ' ' || PEEK(p) == '\t' ||
                                              PEEK(p) == '\n' || PEEK(p) == '\r')) {
                            if (PEEK(p) == '\n') {
                                p->line++;
                                p->col = 1;
                            }
                            ADV(p);
                        }
                    }
                    break;
                default:
                    buf_char(p, c);
                    break;
            }
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

done: {
    XrTomlValue *v = alloc_value(XR_TOML_STRING);
    if (v)
        v->as.string = buf_dup(p);
    return v;
}
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
            XrTomlValue *v = alloc_value(XR_TOML_STRING);
            if (v)
                v->as.string = xr_strdup("");
            return v;
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
    XrTomlValue *v = alloc_value(XR_TOML_STRING);
    if (!v)
        return NULL;
    v->as.string = (char *) xr_malloc(slen + 1);
    if (!v->as.string) {
        xr_free(v);
        return NULL;
    }
    memcpy(v->as.string, start, slen);
    v->as.string[slen] = '\0';
    return v;
}
}

/* ========== Number Parsing ========== */

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
            XrTomlValue *v = alloc_value(XR_TOML_FLOAT);
            if (v)
                v->as.number = negative ? -INFINITY : INFINITY;
            return v;
        }
        if (strncmp(p->data + p->pos, "nan", 3) == 0 &&
            (p->pos + 3 >= p->len || !isalnum((unsigned char) p->data[p->pos + 3]))) {
            p->pos += 3;
            p->col += 3;
            XrTomlValue *v = alloc_value(XR_TOML_FLOAT);
            if (v)
                v->as.number = NAN;
            return v;
        }
    }

    /* Prefix: 0x, 0o, 0b */
    if (p->pos + 1 < p->len && p->data[p->pos] == '0') {
        char nx = p->data[p->pos + 1];
        if (nx == 'x' || nx == 'X') {
            is_hex = true;
            p->pos += 2;
            p->col += 2;
        } else if (nx == 'o' || nx == 'O') {
            is_oct = true;
            p->pos += 2;
            p->col += 2;
        } else if (nx == 'b' || nx == 'B') {
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
    buf_reset(p);
    for (size_t i = 0; i < raw_len; i++) {
        if (start[i] != '_')
            buf_char(p, start[i]);
    }
    buf_char(p, '\0');
    char *num = p->buf;
    (void) p->buf_len; /* num is NUL-terminated in buf */

    if (is_hex || is_oct || is_bin) {
        int base = is_hex ? 16 : (is_oct ? 8 : 2);
        /* Skip sign + "0x"/"0o"/"0b" prefix in stripped buffer */
        const char *digits = num + (negative ? 3 : 2);
        errno = 0;
        int64_t val = strtoll(digits, NULL, base);
        if (errno == ERANGE) {
            p->error = true;
            return NULL;
        }
        XrTomlValue *v = alloc_value(XR_TOML_INTEGER);
        if (v)
            v->as.integer = negative ? -val : val;
        return v;
    }

    if (is_float) {
        XrTomlValue *v = alloc_value(XR_TOML_FLOAT);
        if (v)
            v->as.number = strtod(num, NULL);
        return v;
    }

    /* Decimal integer */
    errno = 0;
    int64_t ival = strtoll(num, NULL, 10);
    if (errno == ERANGE) {
        p->error = true;
        return NULL;
    }
    XrTomlValue *v = alloc_value(XR_TOML_INTEGER);
    if (v)
        v->as.integer = ival;
    return v;
}

/* ========== Datetime Parsing ========== */

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
    XrTomlValue *v = alloc_value(XR_TOML_DATETIME);
    if (!v)
        return NULL;
    v->as.string = (char *) xr_malloc(slen + 1);
    if (!v->as.string) {
        xr_free(v);
        return NULL;
    }
    memcpy(v->as.string, start, slen);
    v->as.string[slen] = '\0';
    return v;
}

/* ========== Array Parsing ========== */

static XrTomlValue *parse_array_value(TomlCtx *p) {
    if (PEEK(p) != '[') {
        p->error = true;
        return NULL;
    }
    ADV(p);

    XrTomlValue *arr = new_array();
    if (!arr)
        return NULL;

    skip_ws_nl(p);

    if (!AT_END(p) && PEEK(p) == ']') {
        ADV(p);
        return arr;
    }

    while (!AT_END(p) && !p->error) {
        skip_ws_nl(p);
        XrTomlValue *val = parse_value(p);
        if (!val) {
            xtoml_free(arr);
            return NULL;
        }
        array_push(arr, val);

        skip_ws_nl(p);
        if (AT_END(p))
            break;
        if (PEEK(p) == ']') {
            ADV(p);
            return arr;
        }
        if (PEEK(p) == ',') {
            ADV(p);
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
static char *parse_key_seg(TomlCtx *p);
/* Forward: parse dotted key path. Returns array of strdup'd keys. */
static char **parse_key_path(TomlCtx *p, int *nkeys);
static void free_keys(char **keys, int n);

static XrTomlValue *parse_inline_table(TomlCtx *p) {
    if (PEEK(p) != '{') {
        p->error = true;
        return NULL;
    }
    ADV(p);

    XrTomlValue *tbl = new_table();
    if (!tbl)
        return NULL;

    skip_ws(p);
    if (!AT_END(p) && PEEK(p) == '}') {
        ADV(p);
        return tbl;
    }

    while (!AT_END(p) && !p->error) {
        skip_ws(p);
        int nkeys = 0;
        char **keys = parse_key_path(p, &nkeys);
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

        XrTomlValue *val = parse_value(p);
        if (!val) {
            free_keys(keys, nkeys);
            xtoml_free(tbl);
            return NULL;
        }
        set_nested(p, tbl, keys, nkeys, val);
        free_keys(keys, nkeys);

        skip_ws(p);
        if (AT_END(p))
            break;
        if (PEEK(p) == '}') {
            ADV(p);
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

static XrTomlValue *parse_value(TomlCtx *p) {
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
        XrTomlValue *v = alloc_value(XR_TOML_BOOL);
        if (v)
            v->as.boolean = true;
        return v;
    }
    if (p->pos + 5 <= p->len && strncmp(p->data + p->pos, "false", 5) == 0 &&
        (p->pos + 5 >= p->len || !isalnum((unsigned char) p->data[p->pos + 5]))) {
        p->pos += 5;
        p->col += 5;
        XrTomlValue *v = alloc_value(XR_TOML_BOOL);
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
            XrTomlValue *v = alloc_value(XR_TOML_FLOAT);
            if (v)
                v->as.number = INFINITY;
            return v;
        }
        if (strncmp(p->data + p->pos, "nan", 3) == 0 &&
            (p->pos + 3 >= p->len || !isalnum((unsigned char) p->data[p->pos + 3]))) {
            p->pos += 3;
            p->col += 3;
            XrTomlValue *v = alloc_value(XR_TOML_FLOAT);
            if (v)
                v->as.number = NAN;
            return v;
        }
    }

    /* Datetime heuristic: YYYY- */
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

/* ========== Key Parsing ========== */

static char *parse_key_seg(TomlCtx *p) {
    if (PEEK(p) == '"') {
        XrTomlValue *sv = parse_basic_string(p);
        if (!sv)
            return NULL;
        char *k = sv->as.string;
        sv->as.string = NULL;
        xr_free(sv);
        return k;
    }
    if (PEEK(p) == '\'') {
        XrTomlValue *sv = parse_literal_string(p);
        if (!sv)
            return NULL;
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
    char *k = (char *) xr_malloc(klen + 1);
    if (!k)
        return NULL;
    memcpy(k, start, klen);
    k[klen] = '\0';
    return k;
}

static void free_keys(char **keys, int n) {
    if (!keys)
        return;
    for (int i = 0; i < n; i++)
        xr_free(keys[i]);
    xr_free(keys);
}

static char **parse_key_path(TomlCtx *p, int *nkeys) {
    *nkeys = 0;
    int cap = 4;
    char **keys = (char **) xr_malloc((size_t) cap * sizeof(char *));
    if (!keys)
        return NULL;

    while (!AT_END(p)) {
        skip_ws(p);
        char *seg = parse_key_seg(p);
        if (!seg) {
            free_keys(keys, *nkeys);
            return NULL;
        }

        if (*nkeys >= cap) {
            cap *= 2;
            char **tmp = (char **) xr_realloc(keys, (size_t) cap * sizeof(char *));
            if (!tmp) {
                xr_free(seg);
                free_keys(keys, *nkeys);
                return NULL;
            }
            keys = tmp;
        }
        keys[(*nkeys)++] = seg;

        skip_ws(p);
        if (AT_END(p) || PEEK(p) != '.')
            break;
        ADV(p); /* skip '.' */
    }
    return keys;
}

/* ========== Nested Value Setting ========== */

/* Get or create nested table along key path, then set the leaf value.
 * Keys array: keys[0..nkeys-2] are intermediate tables, keys[nkeys-1] is leaf. */
static void set_nested(TomlCtx *p, XrTomlValue *root, char **keys, int nkeys, XrTomlValue *val) {
    XrTomlValue *cur = root;
    for (int i = 0; i < nkeys - 1; i++) {
        int idx = table_find(cur, keys[i]);
        if (idx >= 0) {
            XrTomlValue *existing = cur->as.table.members[idx].value;
            if (existing->type == XR_TOML_TABLE) {
                cur = existing;
            } else {
                /* Conflict: overwrite with new table */
                XrTomlValue *nt = new_table();
                if (!nt) {
                    p->error = true;
                    return;
                }
                xtoml_free(existing);
                cur->as.table.members[idx].value = nt;
                cur = nt;
            }
        } else {
            XrTomlValue *nt = new_table();
            if (!nt) {
                p->error = true;
                return;
            }
            table_set(cur, keys[i], nt);
            cur = nt;
        }
    }
    if (nkeys > 0) {
        table_set(cur, keys[nkeys - 1], val);
    }
}

/* ========== Table Header Navigation ========== */

/* Navigate/create path in root, returning the leaf table.
 * Used for [table.path] headers. */
static XrTomlValue *get_or_create_table(XrTomlValue *root, char **keys, int nkeys) {
    XrTomlValue *cur = root;
    for (int i = 0; i < nkeys; i++) {
        int idx = table_find(cur, keys[i]);
        if (idx >= 0) {
            XrTomlValue *existing = cur->as.table.members[idx].value;
            if (existing->type == XR_TOML_TABLE) {
                cur = existing;
            } else if (existing->type == XR_TOML_ARRAY && existing->as.array.count > 0) {
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
            XrTomlValue *nt = new_table();
            if (!nt)
                return NULL;
            table_set(cur, keys[i], nt);
            cur = nt;
        }
    }
    return cur;
}

/* Navigate/create path for [[array.table]] headers.
 * Creates a new table, appends it to the array at the last key. */
static XrTomlValue *get_or_create_array_table(XrTomlValue *root, char **keys, int nkeys) {
    XrTomlValue *cur = root;

    /* Navigate intermediate path */
    for (int i = 0; i < nkeys - 1; i++) {
        int idx = table_find(cur, keys[i]);
        if (idx >= 0) {
            XrTomlValue *existing = cur->as.table.members[idx].value;
            if (existing->type == XR_TOML_TABLE) {
                cur = existing;
            } else if (existing->type == XR_TOML_ARRAY && existing->as.array.count > 0) {
                XrTomlValue *last = existing->as.array.items[existing->as.array.count - 1];
                if (last->type == XR_TOML_TABLE)
                    cur = last;
                else
                    return NULL;
            } else {
                return NULL;
            }
        } else {
            XrTomlValue *nt = new_table();
            if (!nt)
                return NULL;
            table_set(cur, keys[i], nt);
            cur = nt;
        }
    }

    if (nkeys <= 0)
        return cur;

    /* Last key: get or create array, append new table */
    const char *last_key = keys[nkeys - 1];
    int idx = table_find(cur, last_key);
    XrTomlValue *arr;
    if (idx >= 0) {
        arr = cur->as.table.members[idx].value;
        if (arr->type != XR_TOML_ARRAY)
            return NULL;
    } else {
        arr = new_array();
        if (!arr)
            return NULL;
        table_set(cur, last_key, arr);
    }

    XrTomlValue *nt = new_table();
    if (!nt)
        return NULL;
    array_push(arr, nt);
    return nt;
}

/* ========== Main Parse Function ========== */

XR_FUNC XrTomlValue *xtoml_parse(const char *data, size_t len) {
    if (!data)
        return NULL;

    TomlCtx ctx;
    ctx_init(&ctx, data, len);

    XrTomlValue *root = new_table();
    if (!root) {
        ctx_cleanup(&ctx);
        return NULL;
    }

    XrTomlValue *current_table = root;

    while (!AT_END(&ctx) && !ctx.error) {
        skip_ws_nl(&ctx);
        if (AT_END(&ctx))
            break;

        if (PEEK(&ctx) == '[') {
            ADV(&ctx);

            bool is_array_table = false;
            if (!AT_END(&ctx) && PEEK(&ctx) == '[') {
                is_array_table = true;
                ADV(&ctx);
            }

            skip_ws(&ctx);

            int nkeys = 0;
            char **keys = parse_key_path(&ctx, &nkeys);
            if (!keys || nkeys == 0) {
                ctx.error = true;
                free_keys(keys, nkeys);
                break;
            }

            skip_ws(&ctx);

            if (AT_END(&ctx) || PEEK(&ctx) != ']') {
                ctx.error = true;
                free_keys(keys, nkeys);
                break;
            }
            ADV(&ctx);

            if (is_array_table) {
                if (AT_END(&ctx) || PEEK(&ctx) != ']') {
                    ctx.error = true;
                    free_keys(keys, nkeys);
                    break;
                }
                ADV(&ctx);
                current_table = get_or_create_array_table(root, keys, nkeys);
            } else {
                current_table = get_or_create_table(root, keys, nkeys);
            }

            free_keys(keys, nkeys);

            if (!current_table) {
                ctx.error = true;
                break;
            }

            skip_to_eol(&ctx);
            continue;
        }

        /* Key = Value */
        int nkeys = 0;
        char **keys = parse_key_path(&ctx, &nkeys);
        if (!keys || nkeys == 0) {
            ctx.error = true;
            free_keys(keys, nkeys);
            break;
        }

        skip_ws(&ctx);

        if (AT_END(&ctx) || PEEK(&ctx) != '=') {
            ctx.error = true;
            free_keys(keys, nkeys);
            break;
        }
        ADV(&ctx);

        skip_ws(&ctx);

        XrTomlValue *val = parse_value(&ctx);
        if (!val) {
            free_keys(keys, nkeys);
            ctx.error = true;
            break;
        }

        set_nested(&ctx, current_table, keys, nkeys, val);
        free_keys(keys, nkeys);

        skip_to_eol(&ctx);
    }

    ctx_cleanup(&ctx);

    if (ctx.error) {
        xtoml_free(root);
        return NULL;
    }
    return root;
}

/* ========== Accessors ========== */

XR_FUNC XrTomlValue *xtoml_get(XrTomlValue *table, const char *key) {
    if (!table || table->type != XR_TOML_TABLE || !key)
        return NULL;
    int idx = table_find(table, key);
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
