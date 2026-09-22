/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_value_format_core.h - Runtime-neutral value formatting rules.
 */

#ifndef XR_VALUE_FORMAT_CORE_H
#define XR_VALUE_FORMAT_CORE_H

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* These allocation-free helpers are also embedded in generated translation
 * units, where only the helpers required by that program are used. */
#if defined(__GNUC__) || defined(__clang__)
#define XR_VALUE_FORMAT_FUNCTION static inline __attribute__((unused))
#else
#define XR_VALUE_FORMAT_FUNCTION static inline
#endif

#define XR_VALUE_FORMAT_MAX_DEPTH 3
#define XR_VALUE_FORMAT_MAX_ELEMENTS 32

XR_VALUE_FORMAT_FUNCTION int xr_value_format_depth_exceeded(int depth) {
    return depth > XR_VALUE_FORMAT_MAX_DEPTH;
}

XR_VALUE_FORMAT_FUNCTION int64_t xr_value_format_limit_count(int64_t count) {
    if (count <= 0)
        return 0;
    return count > XR_VALUE_FORMAT_MAX_ELEMENTS ? XR_VALUE_FORMAT_MAX_ELEMENTS : count;
}

XR_VALUE_FORMAT_FUNCTION int64_t xr_value_format_remaining_count(int64_t total, int64_t shown) {
    return total > shown ? total - shown : 0;
}

XR_VALUE_FORMAT_FUNCTION int xr_value_format_more_suffix(char *buf, size_t cap, int64_t total, int64_t shown) {
    int64_t remaining = xr_value_format_remaining_count(total, shown);
    if (remaining <= 0) {
        if (buf && cap > 0)
            buf[0] = '\0';
        return 0;
    }
    return snprintf(buf, cap, ", ...(%" PRId64 " more)", remaining);
}


/* Formatting borrows values through backend layout adapters. It never owns a
 * value or builds a second execution representation. */
typedef struct XrValueFormatNode {
    const void *value;
    uint16_t type;
} XrValueFormatNode;

typedef enum XrValueFormatKind {
    XR_VALUE_FORMAT_SIGNED = 1,
    XR_VALUE_FORMAT_UNSIGNED,
    XR_VALUE_FORMAT_BOOL,
    XR_VALUE_FORMAT_BYTES,
    XR_VALUE_FORMAT_ENUM,
    XR_VALUE_FORMAT_LITERAL,
} XrValueFormatKind;

typedef struct XrValueFormatView {
    XrValueFormatKind kind;
    int64_t signed_value;
    uint64_t unsigned_value;
    const char *name;
    const char *member;
    const unsigned char *bytes;
    size_t size;
    uint32_t children;
    char quote;
    unsigned char scalar_bytes[4];
} XrValueFormatView;

typedef struct XrValueFormatReader {
    const void *context;
    int (*read)(const void *, XrValueFormatNode, XrValueFormatView *);
    int (*child)(const void *, XrValueFormatNode, uint32_t, XrValueFormatNode *);
} XrValueFormatReader;

typedef struct XrValueFormatSink {
    void *context;
    int (*write)(void *, const void *, size_t);
} XrValueFormatSink;

XR_VALUE_FORMAT_FUNCTION int xr_value_format_write(XrValueFormatSink sink, const void *bytes, size_t size) {
    return sink.write && (bytes || size == 0u) && sink.write(sink.context, bytes, size);
}

XR_VALUE_FORMAT_FUNCTION int xr_value_format_text(XrValueFormatSink sink, const char *text) {
    return text && xr_value_format_write(sink, text, strlen(text));
}

XR_VALUE_FORMAT_FUNCTION int xr_value_format_file_write(void *context, const void *bytes, size_t size) {
    return context && (size == 0u || fwrite(bytes, 1u, size, (FILE *)context) == size);
}

/* Borrow the message while its panic owner is alive; preserve every byte. */
XR_VALUE_FORMAT_FUNCTION int xr_value_format_panic(XrValueFormatSink sink, uint32_t code,
                                                   int has_bounds, int64_t index, uint64_t length,
                                                   int has_message, const uint8_t *message,
                                                   size_t message_size) {
    char prefix[128];
    int size = snprintf(prefix, sizeof(prefix), "[Uncaught Panic] E%04u: ", code);
    if (size < 0 || (size_t)size >= sizeof(prefix) ||
        !xr_value_format_write(sink, prefix, (size_t)size))
        return 0;
    if (has_message)
        return xr_value_format_write(sink, message, message_size) && xr_value_format_text(sink, "\n");
    if (code == 430u && has_bounds) {
        size = snprintf(prefix, sizeof(prefix), "array index out of range: %" PRId64
                         " (length %" PRIu64 ")\n", index, length);
        return size >= 0 && (size_t)size < sizeof(prefix) &&
               xr_value_format_write(sink, prefix, (size_t)size);
    }
    const char *cause = code == 420u ? "division by zero" :
                        code == 421u ? "modulo by zero" : "uncaught panic";
    return xr_value_format_text(sink, cause) && xr_value_format_text(sink, "\n");
}

XR_VALUE_FORMAT_FUNCTION int xr_value_format_value(XrValueFormatReader reader, XrValueFormatNode node,
                                        XrValueFormatSink sink, int depth) {
    XrValueFormatView view = {0};
    char number[32];
    int length = 0;
    if (!reader.read || !reader.read(reader.context, node, &view))
        return 0;
    switch (view.kind) {
        case XR_VALUE_FORMAT_SIGNED:
            length = snprintf(number, sizeof(number), "%" PRId64, view.signed_value);
            break;
        case XR_VALUE_FORMAT_UNSIGNED:
            length = snprintf(number, sizeof(number), "%" PRIu64, view.unsigned_value);
            break;
        case XR_VALUE_FORMAT_BOOL:
            return xr_value_format_text(sink, view.unsigned_value ? "true" : "false");
        case XR_VALUE_FORMAT_BYTES: {
            int quoted = depth > 0 && view.quote != '\0';
            return (!quoted || xr_value_format_write(sink, &view.quote, 1u)) &&
                   xr_value_format_write(sink, view.bytes, view.size) &&
                   (!quoted || xr_value_format_write(sink, &view.quote, 1u));
        }
        case XR_VALUE_FORMAT_LITERAL:
            return xr_value_format_text(sink, view.name);
        case XR_VALUE_FORMAT_ENUM:
            if (xr_value_format_depth_exceeded(depth))
                return xr_value_format_text(sink, "...");
            if (!xr_value_format_text(sink, view.name ? view.name : "<enum>") ||
                !xr_value_format_text(sink, ".") ||
                !xr_value_format_text(sink, view.member ? view.member : "<variant>"))
                return 0;
            if (!view.children)
                return 1;
            if (!reader.child || !xr_value_format_text(sink, "("))
                return 0;
            for (uint32_t index = 0u; index < view.children; ++index) {
                XrValueFormatNode child = {0};
                if ((index && !xr_value_format_text(sink, ", ")) ||
                    !reader.child(reader.context, node, index, &child) ||
                    !xr_value_format_value(reader, child, sink, depth + 1))
                    return 0;
            }
            return xr_value_format_text(sink, ")");
        default:
            return 0;
    }
    return length > 0 && (size_t)length < sizeof(number) &&
           xr_value_format_write(sink, number, (size_t)length);
}

XR_VALUE_FORMAT_FUNCTION int xr_value_format_uncaught(XrValueFormatReader reader, XrValueFormatNode node,
                                           XrValueFormatSink sink, int in_go) {
    return xr_value_format_text(sink, in_go ? "[Uncaught Error in go coroutine] "
                                           : "[Uncaught Error] ") &&
           xr_value_format_value(reader, node, sink, 0) && xr_value_format_text(sink, "\n");
}

#endif /* XR_VALUE_FORMAT_CORE_H */
