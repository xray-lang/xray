/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_document_position.c - Bounded editor display coordinates
 */
#include "xlsp_server.h"
#include "../../base/xutf8.h"
#include <limits.h>

static bool display_document_valid(const XrLspDocument *doc) {
    return doc && doc->content && doc->line_offsets && doc->line_count > 0 &&
        doc->length <= UINT32_MAX && doc->line_offsets[0] == 0;
}

static bool display_line_span(const XrLspDocument *doc, uint32_t line,
    uint32_t *start, uint32_t *end) {
    uint32_t count = (uint32_t)doc->line_count;
    if (line >= count)
        return false;
    *start = doc->line_offsets[line];
    *end = line + 1u < count ? doc->line_offsets[line + 1u] : (uint32_t)doc->length;
    if (*start > *end || *end > doc->length)
        return false;
    if (*end > *start && doc->content[*end - 1u] == '\n')
        --*end;
    if (*end > *start && doc->content[*end - 1u] == '\r')
        --*end;
    return true;
}

XR_FUNC uint32_t xlsp_position_to_offset(XrLspDocument *doc, XrLspPosition pos) {
    if (!display_document_valid(doc))
        return 0;
    /* Compare unsigned coordinates before any array access or narrowing. */
    if (pos.line >= (uint32_t)doc->line_count)
        return (uint32_t)doc->length;
    uint32_t start, end;
    if (!display_line_span(doc, pos.line, &start, &end))
        return 0;
    size_t column = xr_utf8_utf16_to_byte_offset(doc->content + start,
        (size_t)(end - start), pos.character);
    return start + (uint32_t)column;
}

XR_FUNC XrLspPosition xlsp_offset_to_position(XrLspDocument *doc, uint32_t offset) {
    XrLspPosition pos = {0, 0};
    if (!display_document_valid(doc))
        return pos;
    if (offset > doc->length)
        offset = (uint32_t)doc->length;
    uint32_t low = 0, high = (uint32_t)doc->line_count - 1u;
    while (low < high) {
        uint32_t middle = low + (high - low + 1u) / 2u;
        if (doc->line_offsets[middle] <= offset)
            low = middle;
        else
            high = middle - 1u;
    }
    uint32_t start, end;
    if (!display_line_span(doc, low, &start, &end) || offset < start)
        return pos;
    pos.line = low;
    pos.character = (uint32_t)xr_utf8_byte_to_utf16_offset(doc->content + start,
        (size_t)(end - start), (size_t)(offset - start));
    return pos;
}
