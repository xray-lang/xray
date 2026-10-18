/* xray - Copyright (c) 2026 Xinglei Xu. Licensed under the MIT License. */
#include "app/lsp/xlsp_server.h"
#include "base/xutf8.h"
uint32_t xlsp_position_to_offset(XrLspDocument *doc, XrLspPosition pos) {
    if (!doc || !doc->line_offsets)
        return 0;
    if ((int) pos.line >= doc->line_count) {
        return doc->length;
    }

    uint32_t line_start = doc->line_offsets[pos.line];

    // Calculate line length (bytes until next line or end of document)
    uint32_t line_end;
    if ((int) pos.line + 1 < doc->line_count) {
        line_end = doc->line_offsets[pos.line + 1];
        // Exclude newline character from line content
        if (line_end > line_start && doc->content[line_end - 1] == '\n') {
            line_end--;
        }
    } else {
        line_end = doc->length;
    }
    uint32_t line_len = line_end - line_start;

    // Convert UTF-16 code unit offset to byte offset within the line
    size_t byte_offset_in_line =
        xr_utf8_utf16_to_byte_offset(doc->content + line_start, line_len, pos.character);

    uint32_t offset = line_start + (uint32_t) byte_offset_in_line;
    if (offset > doc->length)
        offset = doc->length;
    return offset;
}

XrLspPosition xlsp_offset_to_position(XrLspDocument *doc, uint32_t offset) {
    XrLspPosition pos = {0, 0};
    if (!doc || !doc->line_offsets)
        return pos;

    // Binary search for line
    int lo = 0, hi = doc->line_count - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (doc->line_offsets[mid] <= offset) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }

    pos.line = lo;

    // Calculate line length for UTF-16 conversion
    uint32_t line_start = doc->line_offsets[lo];
    uint32_t line_end;
    if (lo + 1 < doc->line_count) {
        line_end = doc->line_offsets[lo + 1];
        if (line_end > line_start && doc->content[line_end - 1] == '\n') {
            line_end--;
        }
    } else {
        line_end = doc->length;
    }
    uint32_t line_len = line_end - line_start;

    // Convert byte offset within line to UTF-16 code units
    uint32_t byte_offset_in_line = offset - line_start;
    pos.character = (uint32_t) xr_utf8_byte_to_utf16_offset(doc->content + line_start, line_len,
                                                            byte_offset_in_line);

    return pos;
}
