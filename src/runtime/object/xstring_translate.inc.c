/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xstring_translate.inc.c - Unicode mapping through a private output buffer
 */
XrString *xr_string_translate(XrVMRuntime *iso, XrString *str, XrMap *table) {
    if (!iso || !str)
        return NULL;
    if (!table || str->length == 0)
        return str;

    XrStrBuf *buffer = xr_strbuf_new(iso, 64u);
    if (!buffer)
        return NULL;
    size_t pos = 0;
    while (pos < str->length) {
        size_t char_len = xr_utf8_rune_size((unsigned char) str->data[pos]);
        if (char_len == 0 || pos + char_len > str->length)
            char_len = 1;
        XrString *key = xr_string_new(iso, str->data + pos, char_len);
        if (!key) {
            xr_strbuf_free(buffer);
            return NULL;
        }
        bool found = false;
        XrValue key_value = xr_string_value(key);
        XrValue value = xr_map_get(table, key_value, &found);
        xr_rc_release_value(xr_current_coro_heap(), key_value);
        bool appended = found && XR_IS_STRING(value)
                            ? xr_strbuf_append_str(buffer, XR_TO_STRING(value))
                            : xr_strbuf_append_cstr(buffer, str->data + pos, char_len);
        if (!appended) {
            xr_strbuf_free(buffer);
            return NULL;
        }
        pos += char_len;
    }
    /* Interning copies or independently owns the result before buffer release. */
    XrString *result = xr_strbuf_to_string(buffer);
    xr_strbuf_free(buffer);
    return result;
}
