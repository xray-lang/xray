/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrt_url.h - Freestanding AOT URL helpers
 */

#ifndef XRT_URL_H
#define XRT_URL_H

#include "../shared/xr_url_core.h"
#include "xrt_coll.h"
#include <stdint.h>
#include <string.h>

static inline void xrt_url_buf_append_raw(xrt_strbuf_t *sb, const char *data, size_t len) {
    if (!sb || !data || len == 0)
        return;
    xrt_strbuf_grow(sb, (int64_t) len);
    memcpy(sb->buf + sb->len, data, len);
    sb->len += (int64_t) len;
    sb->buf[sb->len] = '\0';
}

static inline void xrt_url_buf_putc(xrt_strbuf_t *sb, char c) {
    if (!sb)
        return;
    xrt_strbuf_grow(sb, 1);
    sb->buf[sb->len++] = c;
    sb->buf[sb->len] = '\0';
}

static inline bool xrt_url_core_writer_append(void *ctx, const char *data, size_t len) {
    xrt_url_buf_append_raw((xrt_strbuf_t *) ctx, data, len);
    return true;
}

static inline bool xrt_url_core_writer_putc(void *ctx, char c) {
    xrt_url_buf_putc((xrt_strbuf_t *) ctx, c);
    return true;
}

static inline char *xrt_url_core_writer_data(void *ctx) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) ctx;
    return sb ? sb->buf : NULL;
}

static inline size_t xrt_url_core_writer_len(void *ctx) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) ctx;
    return (sb && sb->len > 0) ? (size_t) sb->len : 0;
}

static inline void xrt_url_core_writer_set_len(void *ctx, size_t len) {
    xrt_strbuf_t *sb = (xrt_strbuf_t *) ctx;
    if (!sb)
        return;
    sb->len = (int64_t) len;
    sb->buf[sb->len] = '\0';
}

static inline XrUrlCoreWriter xrt_url_core_writer(xrt_strbuf_t *buf) {
    XrUrlCoreWriter writer;
    writer.ctx = buf;
    writer.append = xrt_url_core_writer_append;
    writer.putc = xrt_url_core_writer_putc;
    writer.data = xrt_url_core_writer_data;
    writer.len = xrt_url_core_writer_len;
    writer.set_len = xrt_url_core_writer_set_len;
    return writer;
}

static inline XrValue xrt_url_str_slice(const char *data, size_t len) {
    XrValue out = xrt_str_alloc(len);
    if (len > 0 && data)
        memcpy(xr_str_buf(out), data, len);
    return out;
}

static inline XrValue xrt_url_encode_impl(const char *data, int64_t len_i, bool form) {
    if (!data && len_i != 0)
        return XR_NULL_VAL;
    size_t len = len_i < 0 ? 0 : (size_t) len_i;
    size_t out_len = 0;
    if (!xr_url_core_encoded_len(data, len, form, &out_len))
        return XR_NULL_VAL;
    XrValue out = xrt_str_alloc(out_len);
    if (!xr_url_core_encode(data, len, form, xr_str_buf(out), &out_len))
        return XR_NULL_VAL;
    return out;
}

static inline XrValue xrt_url_encode(const char *data, int64_t len) {
    return xrt_url_encode_impl(data, len, false);
}

static inline XrValue xrt_url_decode_impl(const char *data, int64_t len_i, bool form) {
    if (!data && len_i != 0)
        return XR_NULL_VAL;
    size_t len = len_i < 0 ? 0 : (size_t) len_i;
    size_t out_len = 0;
    if (!xr_url_core_decoded_len(data, len, &out_len))
        return XR_NULL_VAL;
    XrValue out = xrt_str_alloc(out_len);
    if (!xr_url_core_decode(data, len, form, xr_str_buf(out), &out_len))
        return XR_NULL_VAL;
    return out;
}

static inline XrValue xrt_url_decode(const char *data, int64_t len) {
    return xrt_url_decode_impl(data, len, false);
}

static inline XrValue xrt_url_encode_form(const char *data, int64_t len) {
    return xrt_url_encode_impl(data, len, true);
}

static inline XrValue xrt_url_decode_form(const char *data, int64_t len) {
    return xrt_url_decode_impl(data, len, true);
}

static inline void xrt_url_json_set_slice(XrValue obj, int field, const char *data, size_t len) {
    xrt_json_set_field(obj, field, xrt_url_str_slice(data, data ? len : 0));
}

static inline void xrt_url_map_set_slice(XrValue obj, const char *key, const char *data,
                                         size_t len) {
    xrt_map_set((xrt_map_t *) obj.ptr, xr_box_str(key), xrt_url_str_slice(data, data ? len : 0));
}

static inline void xrt_url_append_host(xrt_strbuf_t *sb, const XrUrlCoreParts *parts,
                                       bool valid_port) {
    if (!sb || !parts)
        return;
    if (parts->hostname && parts->hostname_len > 0)
        xrt_url_buf_append_raw(sb, parts->hostname, parts->hostname_len);
    if (valid_port) {
        xrt_url_buf_putc(sb, ':');
        xrt_url_buf_append_raw(sb, parts->port, parts->port_len);
    }
}

static inline XrValue xrt_url_parse(const char *data, int64_t len_i) {
    size_t len = len_i < 0 ? 0 : (size_t) len_i;
    XrUrlCoreParts parts;
    xr_url_core_parse(data, len, &parts);

    XrValue obj = xrt_map_new(10);
    bool valid_port = xr_url_core_port_is_valid(&parts);

    xrt_url_map_set_slice(obj, "protocol", parts.protocol, parts.protocol_len);
    xrt_url_map_set_slice(obj, "hostname", parts.hostname, parts.hostname_len);
    xrt_url_map_set_slice(obj, "port", valid_port ? parts.port : NULL,
                          valid_port ? parts.port_len : 0);
    if (parts.pathname && parts.pathname_len > 0)
        xrt_url_map_set_slice(obj, "pathname", parts.pathname, parts.pathname_len);
    else
        xrt_url_map_set_slice(obj, "pathname", "/", 1);
    xrt_url_map_set_slice(obj, "search", parts.search, parts.search_len);
    xrt_url_map_set_slice(obj, "hash", parts.hash, parts.hash_len);
    xrt_url_map_set_slice(obj, "username", parts.username, parts.username_len);
    xrt_url_map_set_slice(obj, "password", parts.password, parts.password_len);

    XrValue host_bufv = xrt_strbuf_new();
    xrt_strbuf_t *host_buf = (xrt_strbuf_t *) host_bufv.ptr;
    xrt_url_append_host(host_buf, &parts, valid_port);
    XrValue host = xrt_strbuf_finish(host_bufv);
    xrt_map_set((xrt_map_t *) obj.ptr, xr_box_str("host"), host);

    XrValue origin_bufv = xrt_strbuf_new();
    xrt_strbuf_t *origin_buf = (xrt_strbuf_t *) origin_bufv.ptr;
    if (parts.protocol && parts.protocol_len > 0) {
        xrt_url_buf_append_raw(origin_buf, parts.protocol, parts.protocol_len);
        xrt_url_buf_append_raw(origin_buf, "//", 2);
    }
    xrt_url_buf_append_raw(origin_buf, xr_str_data(host), (size_t) xr_str_len(host));
    xrt_map_set((xrt_map_t *) obj.ptr, xr_box_str("origin"), xrt_strbuf_finish(origin_bufv));
    return obj;
}

static inline XrValue xrt_url_get_name(XrValue obj, const char *name) {
    if (XR_IS_MAP(obj))
        return xrt_map_get((xrt_map_t *) obj.ptr, xr_box_str(name));
    if (obj.tag == XR_TAG_PTR && obj.ptr)
        return xrt_json_get_name(obj, name);
    return XR_NULL_VAL;
}

static inline bool xrt_url_format_field(void *ctx, const char *name, const char **data,
                                        size_t *len) {
    XrValue obj = *(XrValue *) ctx;
    XrValue value = xrt_url_get_name(obj, name);
    if (XR_IS_STR(value)) {
        *data = xr_str_data(value);
        *len = (size_t) xr_str_len(value);
    } else {
        *data = NULL;
        *len = 0;
    }
    return true;
}

static inline XrValue xrt_url_format(XrValue obj) {
    if (obj.tag != XR_TAG_PTR && !XR_IS_MAP(obj))
        return XR_NULL_VAL;
    XrValue bufv = xrt_strbuf_new();
    xrt_strbuf_t *buf = (xrt_strbuf_t *) bufv.ptr;
    XrUrlCoreWriter writer = xrt_url_core_writer(buf);
    if (!xr_url_core_format_write(xrt_url_format_field, &obj, &writer)) {
        xrt_release(bufv);
        return XR_NULL_VAL;
    }
    return xrt_strbuf_finish(bufv);
}

static inline void xrt_url_json_set_dynamic(XrValue obj, XrValue key, XrValue value) {
    xrt_json_t *j = (xrt_json_t *) obj.ptr;
    if (!j->dynamic_fields) {
        XrValue dyn = xrt_map_new(8);
        j->dynamic_fields = (xrt_map_t *) dyn.ptr;
    }
    xrt_map_set(j->dynamic_fields, key, value);
}

static inline XrValue xrt_url_parse_query(const char *data, int64_t len_i) {
    size_t len = len_i < 0 ? 0 : (size_t) len_i;
    if (!data && len != 0)
        return XR_NULL_VAL;
    if (len > 0 && data[0] == '?') {
        data++;
        len--;
    }

    XrValue obj = xrt_json_new(0);
    if (len == 0)
        return obj;

    const char *p = data;
    const char *end = data + len;
    while (p < end) {
        const char *amp = memchr(p, '&', (size_t) (end - p));
        const char *pair_end = amp ? amp : end;
        const char *eq = memchr(p, '=', (size_t) (pair_end - p));
        const char *key_start = p;
        size_t key_len = eq ? (size_t) (eq - key_start) : (size_t) (pair_end - key_start);
        const char *val_start = eq ? eq + 1 : NULL;
        size_t val_len = eq ? (size_t) (pair_end - val_start) : 0;

        if (key_len > 0) {
            size_t decoded_key_len = 0;
            xr_url_core_decoded_len(key_start, key_len, &decoded_key_len);
            XrValue key = xrt_str_alloc(decoded_key_len);
            xr_url_core_decode(key_start, key_len, true, xr_str_buf(key), &decoded_key_len);

            XrValue val;
            if (val_start) {
                size_t decoded_val_len = 0;
                xr_url_core_decoded_len(val_start, val_len, &decoded_val_len);
                val = xrt_str_alloc(decoded_val_len);
                xr_url_core_decode(val_start, val_len, true, xr_str_buf(val), &decoded_val_len);
            } else {
                val = xrt_str_alloc(0);
            }
            xrt_url_json_set_dynamic(obj, key, val);
        }
        p = amp ? amp + 1 : end;
    }
    return obj;
}

static inline void xrt_url_append_encoded_form(xrt_strbuf_t *buf, const char *data, size_t len) {
    if (!buf || !data || len == 0)
        return;
    size_t encoded_len = 0;
    if (!xr_url_core_encoded_len(data, len, true, &encoded_len))
        return;
    xrt_strbuf_grow(buf, (int64_t) encoded_len);
    size_t written = 0;
    xr_url_core_encode(data, len, true, buf->buf + buf->len, &written);
    buf->len += (int64_t) written;
    buf->buf[buf->len] = '\0';
}

static inline void xrt_url_build_query_pair(xrt_strbuf_t *buf, const char *key, size_t key_len,
                                            XrValue val) {
    if (!buf || !key)
        return;
    if (buf->len > 0)
        xrt_url_buf_putc(buf, '&');
    xrt_url_append_encoded_form(buf, key, key_len);
    if (XR_IS_NULL(val))
        return;
    xrt_url_buf_putc(buf, '=');
    if (XR_IS_STR(val)) {
        xrt_url_append_encoded_form(buf, xr_str_data(val), (size_t) xr_str_len(val));
    } else {
        char tmp[128];
        const char *s = xr_to_cstr(val, tmp, sizeof(tmp));
        xrt_url_append_encoded_form(buf, s, strlen(s));
    }
}

static inline void xrt_url_build_query_json_fields(xrt_strbuf_t *buf, xrt_json_t *j) {
    if (!buf || !j)
        return;
    for (int64_t i = 0; i < j->field_count; i++) {
        const char *key = j->field_names ? j->field_names[i] : NULL;
        if (key)
            xrt_url_build_query_pair(buf, key, strlen(key), j->fields[i]);
    }
    if (!j->dynamic_fields)
        return;
    xrt_map_t *m = j->dynamic_fields;
    for (uint32_t i = 0; i < m->nentries; i++) {
        XrMapEntry *entry = &m->entries[i];
        if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
            continue;
        char key_buf[128];
        const char *key = XR_IS_STR(entry->key) ? xr_str_data(entry->key)
                                                : xr_to_cstr(entry->key, key_buf, sizeof(key_buf));
        size_t key_len = XR_IS_STR(entry->key) ? (size_t) xr_str_len(entry->key) : strlen(key);
        xrt_url_build_query_pair(buf, key, key_len, entry->value);
    }
}

static inline void xrt_url_build_query_map_fields(xrt_strbuf_t *buf, xrt_map_t *m) {
    if (!buf || !m)
        return;
    if (!xrt_map_is_typed(m)) {
        for (uint32_t i = 0; i < m->nentries; i++) {
            XrMapEntry *entry = &m->entries[i];
            if (entry->key_tt == XR_MAP_ENTRY_NIL_KEY)
                continue;
            char key_buf[128];
            const char *key = XR_IS_STR(entry->key)
                                  ? xr_str_data(entry->key)
                                  : xr_to_cstr(entry->key, key_buf, sizeof(key_buf));
            size_t key_len = XR_IS_STR(entry->key) ? (size_t) xr_str_len(entry->key) : strlen(key);
            xrt_url_build_query_pair(buf, key, key_len, entry->value);
        }
        return;
    }
    for (int64_t oi = 0; oi < m->order_len; oi++) {
        int64_t slot = m->order[oi];
        if (!xrt_map_slot_is_full(m, slot))
            continue;
        XrValue keyv = xrt_map_slot_key(m, slot);
        XrValue val = xrt_map_slot_value(m, slot);
        char key_buf[128];
        const char *key =
            XR_IS_STR(keyv) ? xr_str_data(keyv) : xr_to_cstr(keyv, key_buf, sizeof(key_buf));
        size_t key_len = XR_IS_STR(keyv) ? (size_t) xr_str_len(keyv) : strlen(key);
        xrt_url_build_query_pair(buf, key, key_len, val);
    }
}

static inline XrValue xrt_url_build_query(XrValue obj) {
    if (obj.tag != XR_TAG_PTR && !XR_IS_MAP(obj))
        return XR_NULL_VAL;
    XrValue bufv = xrt_strbuf_new();
    xrt_strbuf_t *buf = (xrt_strbuf_t *) bufv.ptr;
    if (XR_IS_MAP(obj))
        xrt_url_build_query_map_fields(buf, (xrt_map_t *) obj.ptr);
    else
        xrt_url_build_query_json_fields(buf, (xrt_json_t *) obj.ptr);
    return xrt_strbuf_finish(bufv);
}

static inline XrValue xrt_url_resolve(const char *base, int64_t base_len_i, const char *rel,
                                      int64_t rel_len_i) {
    size_t base_len = base_len_i < 0 ? 0 : (size_t) base_len_i;
    size_t rel_len = rel_len_i < 0 ? 0 : (size_t) rel_len_i;
    if ((!base && base_len != 0) || (!rel && rel_len != 0))
        return XR_NULL_VAL;

    XrValue resultv = xrt_strbuf_new();
    xrt_strbuf_t *result = (xrt_strbuf_t *) resultv.ptr;
    XrUrlCoreWriter writer = xrt_url_core_writer(result);
    if (!xr_url_core_resolve_write(base, base_len, rel, rel_len, &writer)) {
        xrt_release(resultv);
        return XR_NULL_VAL;
    }
    return xrt_strbuf_finish(resultv);
}

typedef struct {
    const char **parts;
    const size_t *lens;
} xrt_url_join_parts_t;

static inline bool xrt_url_join_part(void *ctx, size_t index, const char **data, size_t *len) {
    xrt_url_join_parts_t *parts = (xrt_url_join_parts_t *) ctx;
    if (!parts || !data || !len)
        return false;
    *data = parts->parts ? parts->parts[index] : NULL;
    *len = parts->lens ? parts->lens[index] : 0;
    return true;
}

static inline XrValue xrt_url_join(int64_t count_i, const char **parts, const size_t *lens) {
    XrValue bufv = xrt_strbuf_new();
    xrt_strbuf_t *buf = (xrt_strbuf_t *) bufv.ptr;
    xrt_url_join_parts_t join_parts;
    join_parts.parts = parts;
    join_parts.lens = lens;
    XrUrlCoreWriter writer = xrt_url_core_writer(buf);
    size_t count = count_i <= 0 ? 0 : (size_t) count_i;
    if (!xr_url_core_join_write(count, xrt_url_join_part, &join_parts, &writer)) {
        xrt_release(bufv);
        return XR_NULL_VAL;
    }
    return xrt_strbuf_finish(bufv);
}

#endif  // XRT_URL_H
