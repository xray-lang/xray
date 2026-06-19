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
