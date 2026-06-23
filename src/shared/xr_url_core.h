/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_url_core.h - Pure URL helpers shared by VM stdlib and AOT
 */

#ifndef XR_URL_CORE_H
#define XR_URL_CORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    const char *protocol;
    size_t protocol_len;
    const char *username;
    size_t username_len;
    const char *password;
    size_t password_len;
    const char *hostname;
    size_t hostname_len;
    const char *port;
    size_t port_len;
    const char *pathname;
    size_t pathname_len;
    const char *search;
    size_t search_len;
    const char *hash;
    size_t hash_len;
} XrUrlCoreParts;

typedef struct {
    void *ctx;
    bool (*append)(void *ctx, const char *data, size_t len);
    bool (*putc)(void *ctx, char c);
    char *(*data)(void *ctx);
    size_t (*len)(void *ctx);
    void (*set_len)(void *ctx, size_t len);
} XrUrlCoreWriter;

typedef bool (*XrUrlCoreJoinPartFn)(void *ctx, size_t index, const char **data, size_t *len);
typedef bool (*XrUrlCoreFieldFn)(void *ctx, const char *name, const char **data, size_t *len);

static inline uint8_t xr_url_core_hex_value(unsigned char c) {
    if (c >= '0' && c <= '9')
        return (uint8_t) (c - '0');
    if (c >= 'a' && c <= 'f')
        return (uint8_t) (10 + c - 'a');
    if (c >= 'A' && c <= 'F')
        return (uint8_t) (10 + c - 'A');
    return 255;
}

static inline bool xr_url_core_is_unreserved(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

static inline bool xr_url_core_add_size(size_t *acc, size_t delta) {
    if (!acc || *acc > SIZE_MAX - delta)
        return false;
    *acc += delta;
    return true;
}

static inline bool xr_url_core_encoded_len(const char *str, size_t len, bool form,
                                           size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!str && len != 0)
        return false;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) str[i];
        size_t delta = (xr_url_core_is_unreserved(c) || (form && c == ' ')) ? 1u : 3u;
        if (!xr_url_core_add_size(&n, delta))
            return false;
    }
    if (out_len)
        *out_len = n;
    return true;
}

static inline bool xr_url_core_encode(const char *str, size_t len, bool form, char *out,
                                      size_t *out_len) {
    static const char hex_chars[] = "0123456789ABCDEF";
    if ((!str && len != 0) || !out)
        return false;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) str[i];
        if (form && c == ' ') {
            out[pos++] = '+';
        } else if (xr_url_core_is_unreserved(c)) {
            out[pos++] = (char) c;
        } else {
            out[pos++] = '%';
            out[pos++] = hex_chars[(c >> 4) & 0x0F];
            out[pos++] = hex_chars[c & 0x0F];
        }
    }
    out[pos] = '\0';
    if (out_len)
        *out_len = pos;
    return true;
}

static inline int xr_url_core_encode_bounded(const char *str, size_t len, bool form, char *buf,
                                             size_t buf_size) {
    static const char hex_chars[] = "0123456789ABCDEF";
    if (!buf || buf_size == 0)
        return 0;
    if (!str && len != 0) {
        buf[0] = '\0';
        return 0;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) str[i];
        if (form && c == ' ') {
            if (j + 1 >= buf_size)
                break;
            buf[j++] = '+';
        } else if (xr_url_core_is_unreserved(c)) {
            if (j + 1 >= buf_size)
                break;
            buf[j++] = (char) c;
        } else {
            if (j + 3 >= buf_size)
                break;
            buf[j++] = '%';
            buf[j++] = hex_chars[(c >> 4) & 0x0F];
            buf[j++] = hex_chars[c & 0x0F];
        }
    }
    buf[j] = '\0';
    return (int) j;
}

static inline bool xr_url_core_decoded_len(const char *str, size_t len, size_t *out_len) {
    if (out_len)
        *out_len = 0;
    if (!str && len != 0)
        return false;
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '%' && i + 2 < len) {
            uint8_t hi = xr_url_core_hex_value((unsigned char) str[i + 1]);
            uint8_t lo = xr_url_core_hex_value((unsigned char) str[i + 2]);
            if (hi != 255 && lo != 255)
                i += 2;
        }
        if (!xr_url_core_add_size(&n, 1))
            return false;
    }
    if (out_len)
        *out_len = n;
    return true;
}

static inline bool xr_url_core_decode(const char *str, size_t len, bool form, char *out,
                                      size_t *out_len) {
    if ((!str && len != 0) || !out)
        return false;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] == '%' && i + 2 < len) {
            uint8_t hi = xr_url_core_hex_value((unsigned char) str[i + 1]);
            uint8_t lo = xr_url_core_hex_value((unsigned char) str[i + 2]);
            if (hi != 255 && lo != 255) {
                out[j++] = (char) ((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out[j++] = (form && str[i] == '+') ? ' ' : str[i];
    }
    out[j] = '\0';
    if (out_len)
        *out_len = j;
    return true;
}

static inline int xr_url_core_decode_bounded(const char *str, size_t len, bool form, char *buf,
                                             size_t buf_size) {
    if (!buf || buf_size == 0)
        return 0;
    if (!str && len != 0) {
        buf[0] = '\0';
        return 0;
    }
    size_t j = 0;
    for (size_t i = 0; i < len && j < buf_size - 1; i++) {
        if (str[i] == '%' && i + 2 < len) {
            uint8_t hi = xr_url_core_hex_value((unsigned char) str[i + 1]);
            uint8_t lo = xr_url_core_hex_value((unsigned char) str[i + 2]);
            if (hi != 255 && lo != 255) {
                buf[j++] = (char) ((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        buf[j++] = (form && str[i] == '+') ? ' ' : str[i];
    }
    buf[j] = '\0';
    return (int) j;
}

static inline void xr_url_core_parse(const char *url, size_t url_len, XrUrlCoreParts *out) {
    memset(out, 0, sizeof(*out));
    if (!url || url_len == 0)
        return;

    const char *p = url;
    const char *end = url + url_len;

    const char *colon = memchr(p, ':', (size_t) (end - p));
    if (colon && colon + 2 < end && colon[1] == '/' && colon[2] == '/') {
        out->protocol = p;
        out->protocol_len = (size_t) (colon - p + 1);
        p = colon + 3;
    }

    const char *authority_end = end;
    for (const char *c = p; c < end; c++) {
        if (*c == '/' || *c == '?' || *c == '#') {
            authority_end = c;
            break;
        }
    }

    const char *auth_start = p;
    const char *auth_end = authority_end;
    const char *at = NULL;
    for (const char *c = auth_start; c < auth_end; c++) {
        if (*c == '@') {
            at = c;
            break;
        }
    }

    const char *host_start;
    if (at) {
        const char *user_colon = memchr(auth_start, ':', (size_t) (at - auth_start));
        out->username = auth_start;
        if (user_colon) {
            out->username_len = (size_t) (user_colon - auth_start);
            out->password = user_colon + 1;
            out->password_len = (size_t) (at - user_colon - 1);
        } else {
            out->username_len = (size_t) (at - auth_start);
        }
        host_start = at + 1;
    } else {
        host_start = auth_start;
    }

    if (host_start < auth_end && *host_start == '[') {
        const char *bracket = memchr(host_start, ']', (size_t) (auth_end - host_start));
        if (bracket) {
            out->hostname = host_start;
            out->hostname_len = (size_t) (bracket - host_start + 1);
            if (bracket + 1 < auth_end && bracket[1] == ':') {
                out->port = bracket + 2;
                out->port_len = (size_t) (auth_end - bracket - 2);
            }
        } else {
            out->hostname = host_start;
            out->hostname_len = (size_t) (auth_end - host_start);
        }
    } else {
        const char *port_colon = NULL;
        for (const char *c = host_start; c < auth_end; c++) {
            if (*c == ':')
                port_colon = c;
        }
        if (port_colon) {
            out->hostname = host_start;
            out->hostname_len = (size_t) (port_colon - host_start);
            out->port = port_colon + 1;
            out->port_len = (size_t) (auth_end - port_colon - 1);
        } else {
            out->hostname = host_start;
            out->hostname_len = (size_t) (auth_end - host_start);
        }
    }

    p = authority_end;
    if (p < end && *p == '/') {
        const char *path_end = end;
        for (const char *c = p; c < end; c++) {
            if (*c == '?' || *c == '#') {
                path_end = c;
                break;
            }
        }
        out->pathname = p;
        out->pathname_len = (size_t) (path_end - p);
        p = path_end;
    }

    if (p < end && *p == '?') {
        const char *search_end = end;
        for (const char *c = p; c < end; c++) {
            if (*c == '#') {
                search_end = c;
                break;
            }
        }
        out->search = p;
        out->search_len = (size_t) (search_end - p);
        p = search_end;
    }

    if (p < end && *p == '#') {
        out->hash = p;
        out->hash_len = (size_t) (end - p);
    }
}

static inline bool xr_url_core_port_is_valid(const XrUrlCoreParts *parts) {
    if (!parts || !parts->port || parts->port_len == 0 || parts->port_len > 5)
        return false;
    uint32_t port_val = 0;
    for (size_t i = 0; i < parts->port_len; i++) {
        char c = parts->port[i];
        if (c < '0' || c > '9')
            return false;
        port_val = port_val * 10u + (uint32_t) (c - '0');
    }
    return port_val <= 65535u;
}

static inline bool xr_url_core_writer_append(XrUrlCoreWriter *w, const char *data, size_t len) {
    if (len == 0)
        return true;
    return w && w->append && w->append(w->ctx, data, len);
}

static inline bool xr_url_core_writer_putc(XrUrlCoreWriter *w, char c) {
    return w && w->putc && w->putc(w->ctx, c);
}

static inline char *xr_url_core_writer_data(XrUrlCoreWriter *w) {
    return (w && w->data) ? w->data(w->ctx) : NULL;
}

static inline size_t xr_url_core_writer_len(XrUrlCoreWriter *w) {
    return (w && w->len) ? w->len(w->ctx) : 0;
}

static inline void xr_url_core_writer_set_len(XrUrlCoreWriter *w, size_t len) {
    if (w && w->set_len)
        w->set_len(w->ctx, len);
}

static inline bool xr_url_core_emit_base_authority(XrUrlCoreWriter *out,
                                                   const XrUrlCoreParts *parts) {
    if (!out || !parts)
        return false;
    if (parts->protocol && parts->protocol_len > 0) {
        if (!xr_url_core_writer_append(out, parts->protocol, parts->protocol_len) ||
            !xr_url_core_writer_append(out, "//", 2))
            return false;
    }
    if (parts->hostname && parts->hostname_len > 0) {
        if (!xr_url_core_writer_append(out, parts->hostname, parts->hostname_len))
            return false;
    }
    if (parts->port && parts->port_len > 0) {
        if (!xr_url_core_writer_putc(out, ':') ||
            !xr_url_core_writer_append(out, parts->port, parts->port_len))
            return false;
    }
    return true;
}

static inline bool xr_url_core_get_field(XrUrlCoreFieldFn field, void *field_ctx, const char *name,
                                         const char **data, size_t *len) {
    if (data)
        *data = NULL;
    if (len)
        *len = 0;
    return field && data && len && field(field_ctx, name, data, len);
}

static inline bool xr_url_core_append_field(XrUrlCoreWriter *out, const char *data, size_t len) {
    return (!data || len == 0) ? true : xr_url_core_writer_append(out, data, len);
}

static inline bool xr_url_core_format_write(XrUrlCoreFieldFn field, void *field_ctx,
                                            XrUrlCoreWriter *out) {
    if (!field || !out)
        return false;

    const char *protocol = NULL;
    const char *hostname = NULL;
    const char *port = NULL;
    const char *pathname = NULL;
    const char *search = NULL;
    const char *hash = NULL;
    const char *username = NULL;
    const char *password = NULL;
    size_t protocol_len = 0;
    size_t hostname_len = 0;
    size_t port_len = 0;
    size_t pathname_len = 0;
    size_t search_len = 0;
    size_t hash_len = 0;
    size_t username_len = 0;
    size_t password_len = 0;

    if (!xr_url_core_get_field(field, field_ctx, "protocol", &protocol, &protocol_len) ||
        !xr_url_core_get_field(field, field_ctx, "hostname", &hostname, &hostname_len) ||
        !xr_url_core_get_field(field, field_ctx, "port", &port, &port_len) ||
        !xr_url_core_get_field(field, field_ctx, "pathname", &pathname, &pathname_len) ||
        !xr_url_core_get_field(field, field_ctx, "search", &search, &search_len) ||
        !xr_url_core_get_field(field, field_ctx, "hash", &hash, &hash_len) ||
        !xr_url_core_get_field(field, field_ctx, "username", &username, &username_len) ||
        !xr_url_core_get_field(field, field_ctx, "password", &password, &password_len))
        return false;

    if (protocol && protocol_len > 0) {
        if (!xr_url_core_writer_append(out, protocol, protocol_len) ||
            !xr_url_core_writer_append(out, "//", 2))
            return false;
    }

    if (username && username_len > 0) {
        if (!xr_url_core_writer_append(out, username, username_len))
            return false;
        if (password && password_len > 0) {
            if (!xr_url_core_writer_putc(out, ':') ||
                !xr_url_core_writer_append(out, password, password_len))
                return false;
        }
        if (!xr_url_core_writer_putc(out, '@'))
            return false;
    }

    if (!xr_url_core_append_field(out, hostname, hostname_len))
        return false;
    if (port && port_len > 0) {
        if (!xr_url_core_writer_putc(out, ':') || !xr_url_core_writer_append(out, port, port_len))
            return false;
    }
    return xr_url_core_append_field(out, pathname, pathname_len) &&
           xr_url_core_append_field(out, search, search_len) &&
           xr_url_core_append_field(out, hash, hash_len);
}

static inline size_t xr_url_core_remove_dot_segments(char *path, size_t len);

static inline bool xr_url_core_resolve_write(const char *base, size_t base_len, const char *rel,
                                             size_t rel_len, XrUrlCoreWriter *out) {
    if ((!base && base_len != 0) || (!rel && rel_len != 0) || !out)
        return false;

    const char *colon = memchr(rel, ':', rel_len);
    if (colon && colon + 2 < rel + rel_len && colon[1] == '/' && colon[2] == '/')
        return xr_url_core_writer_append(out, rel, rel_len);

    XrUrlCoreParts bp;
    xr_url_core_parse(base, base_len, &bp);

    const char *rel_hash = memchr(rel, '#', rel_len);
    size_t rel_hash_len = rel_hash ? (size_t) ((rel + rel_len) - rel_hash) : 0;
    size_t rel_no_hash_len = rel_hash ? (size_t) (rel_hash - rel) : rel_len;

    const char *rel_query = memchr(rel, '?', rel_no_hash_len);
    size_t rel_query_len = 0;
    size_t rel_path_len = rel_no_hash_len;
    if (rel_query) {
        rel_query_len = (size_t) ((rel + rel_no_hash_len) - rel_query);
        rel_path_len = (size_t) (rel_query - rel);
    }

    size_t path_start = 0;
    size_t path_end = 0;

    if (rel_len > 1 && rel[0] == '/' && rel[1] == '/') {
        if (bp.protocol && bp.protocol_len > 0) {
            if (!xr_url_core_writer_append(out, bp.protocol, bp.protocol_len))
                return false;
        }
        if (!xr_url_core_writer_append(out, rel, 2))
            return false;
        size_t authority_end = 2;
        while (authority_end < rel_no_hash_len && rel[authority_end] != '/' &&
               rel[authority_end] != '?')
            authority_end++;
        if (!xr_url_core_writer_append(out, rel + 2, authority_end - 2))
            return false;
        path_start = xr_url_core_writer_len(out);
        if (rel_path_len > authority_end &&
            !xr_url_core_writer_append(out, rel + authority_end, rel_path_len - authority_end))
            return false;
        path_end = xr_url_core_writer_len(out);
    } else if (rel_path_len > 0 && rel[0] == '/') {
        if (!xr_url_core_emit_base_authority(out, &bp))
            return false;
        path_start = xr_url_core_writer_len(out);
        if (!xr_url_core_writer_append(out, rel, rel_path_len))
            return false;
        path_end = xr_url_core_writer_len(out);
    } else if (rel_path_len == 0) {
        if (!xr_url_core_emit_base_authority(out, &bp))
            return false;
        path_start = xr_url_core_writer_len(out);
        if (bp.pathname && bp.pathname_len > 0 &&
            !xr_url_core_writer_append(out, bp.pathname, bp.pathname_len))
            return false;
        path_end = xr_url_core_writer_len(out);
        if (!rel_query && bp.search && bp.search_len > 0 &&
            !xr_url_core_writer_append(out, bp.search, bp.search_len))
            return false;
    } else {
        if (!xr_url_core_emit_base_authority(out, &bp))
            return false;
        path_start = xr_url_core_writer_len(out);
        if (bp.pathname && bp.pathname_len > 0) {
            const char *last_slash = NULL;
            for (size_t i = bp.pathname_len; i > 0; i--) {
                if (bp.pathname[i - 1] == '/') {
                    last_slash = &bp.pathname[i - 1];
                    break;
                }
            }
            if (last_slash) {
                size_t prefix = (size_t) (last_slash - bp.pathname + 1);
                if (!xr_url_core_writer_append(out, bp.pathname, prefix))
                    return false;
            } else if (!xr_url_core_writer_putc(out, '/')) {
                return false;
            }
        } else if (!xr_url_core_writer_putc(out, '/')) {
            return false;
        }
        if (!xr_url_core_writer_append(out, rel, rel_path_len))
            return false;
        path_end = xr_url_core_writer_len(out);
    }

    if (rel_query && !xr_url_core_writer_append(out, rel_query, rel_query_len))
        return false;
    if (rel_hash && !xr_url_core_writer_append(out, rel_hash, rel_hash_len))
        return false;

    if (path_end > path_start) {
        char *data = xr_url_core_writer_data(out);
        if (!data)
            return false;
        size_t old_path_len = path_end - path_start;
        size_t total_len = xr_url_core_writer_len(out);
        size_t tail_len = total_len - path_end;
        char tail_first = tail_len > 0 ? data[path_end] : '\0';
        size_t new_path_len = xr_url_core_remove_dot_segments(data + path_start, old_path_len);
        size_t shrink = old_path_len - new_path_len;
        if (shrink > 0) {
            if (tail_len > 0)
                memmove(data + path_start + new_path_len, data + path_end, tail_len);
            xr_url_core_writer_set_len(out, total_len - shrink);
        } else if (tail_len > 0) {
            data[path_end] = tail_first;
        }
    }

    return true;
}

static inline bool xr_url_core_join_write(size_t count, XrUrlCoreJoinPartFn part, void *part_ctx,
                                          XrUrlCoreWriter *out) {
    if (!out || (count > 0 && !part))
        return false;

    for (size_t i = 0; i < count; i++) {
        const char *seg = NULL;
        size_t seg_len = 0;
        if (!part(part_ctx, i, &seg, &seg_len))
            return false;
        if (!seg || seg_len == 0)
            continue;

        size_t out_len = xr_url_core_writer_len(out);
        if (out_len > 0) {
            char *data = xr_url_core_writer_data(out);
            if (!data)
                return false;
            if (data[out_len - 1] == '/') {
                xr_url_core_writer_set_len(out, out_len - 1);
                out_len--;
            }
        }

        if (out_len > 0 && seg[0] != '/' && !xr_url_core_writer_putc(out, '/'))
            return false;
        if (!xr_url_core_writer_append(out, seg, seg_len))
            return false;
    }

    return true;
}

static inline size_t xr_url_core_remove_dot_segments(char *path, size_t len) {
    if (!path || len == 0)
        return 0;
    char *out = path;
    char *in = path;
    char *end = path + len;

    while (in < end) {
        if (in + 3 <= end && in[0] == '.' && in[1] == '.' && in[2] == '/') {
            in += 3;
            continue;
        }
        if (in + 2 <= end && in[0] == '.' && in[1] == '/') {
            in += 2;
            continue;
        }
        if (in + 3 <= end && in[0] == '/' && in[1] == '.' && in[2] == '/') {
            in += 2;
            continue;
        }
        if (in + 2 == end && in[0] == '/' && in[1] == '.') {
            *out++ = '/';
            in = end;
            continue;
        }
        if (in + 4 <= end && in[0] == '/' && in[1] == '.' && in[2] == '.' && in[3] == '/') {
            in += 3;
            if (out > path) {
                out--;
                while (out > path && *out != '/')
                    out--;
            }
            continue;
        }
        if (in + 3 == end && in[0] == '/' && in[1] == '.' && in[2] == '.') {
            in = end;
            if (out > path) {
                out--;
                while (out > path && *out != '/')
                    out--;
            }
            *out++ = '/';
            continue;
        }
        if ((in + 1 == end && in[0] == '.') || (in + 2 == end && in[0] == '.' && in[1] == '.')) {
            in = end;
            continue;
        }
        if (*in == '/')
            *out++ = *in++;
        while (in < end && *in != '/')
            *out++ = *in++;
    }
    *out = '\0';
    return (size_t) (out - path);
}

#endif  // XR_URL_CORE_H
