/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_buffer.inc.c - C emission with charged byte reads and writes
 *
 * KEY CONCEPT:
 *   Both sizing and writing consume the same finite ledger and byte bound.
 */
typedef struct CBuffer {
    char *text;
    size_t length, capacity, limit;
    XrXirStatus status;
    const XrXirTypes *types;
    const XrXirCompileContext *context;
    bool measuring, tracking;
    unsigned label_match[2];
    bool label_used[2];
} CBuffer;

static bool emit_work(CBuffer *buffer, uint64_t units) {
    if (buffer->status != XR_XIR_OK) return false;
    if (xir_compile_work(buffer->context, units)) return true;
    buffer->status = XR_XIR_BUDGET;
    return false;
}
/* The tracked patterns have no overlapping prefix other than their first
 * byte. Match the emitted chunk itself, without storing a second byte stream. */
static bool emit_label_byte(CBuffer *buffer, char byte) {
    static const char *const references[2] = { /* owned: static string literals */
        "goto invalid;", "goto limit;"
    };
    static const unsigned lengths[2] = {sizeof("goto invalid;") - 1, sizeof("goto limit;") - 1};
    if (!buffer->tracking) return true;
    if (!emit_work(buffer, 2)) return false;
    if (!buffer->label_match[0] && !buffer->label_match[1] && byte != 'g') return true;
    for (unsigned i = 0; i < 2; ++i) {
        if (!emit_work(buffer, 1)) return false;
        if (buffer->label_used[i]) continue;
        if (!emit_work(buffer, 2)) return false;
        unsigned match = buffer->label_match[i];
        if (byte == references[i][match]) ++match;
        else {
            if (!emit_work(buffer, 1)) return false;
            match = byte == references[i][0] ? 1u : 0u;
        }
        if (!emit_work(buffer, 2)) return false;
        bool used = match == lengths[i];
        buffer->label_match[i] = used ? 0 : match;
        buffer->label_used[i] = used;
        if (used) {
            if (!emit_work(buffer, 2)) return false;
            if (buffer->label_used[0] && buffer->label_used[1]) {
                if (!emit_work(buffer, 1)) return false;
                buffer->tracking = false;
                return true;
            }
        }
    }
    return true;
}
static void emit_byte(CBuffer *buffer, char byte) {
    if (buffer->status != XR_XIR_OK) return;
    if (buffer->length > buffer->limit || buffer->limit - buffer->length < 2) {
        buffer->status = XR_XIR_BUDGET;
        return;
    }
    if (buffer->measuring) {
        if (!emit_work(buffer, 1)) return;
        ++buffer->length;
        (void) emit_label_byte(buffer, byte);
        return;
    }
    size_t required = buffer->length + 2;
    if (required > buffer->capacity) {
        size_t capacity = buffer->capacity ? buffer->capacity :
            (buffer->limit < 128 ? buffer->limit : 128);
        while (capacity < required && capacity <= buffer->limit / 2) {
            if (!emit_work(buffer, 1)) return;
            capacity *= 2;
        }
        if (capacity < required || capacity > buffer->limit) capacity = required;
        void *memory = buffer->text;
        buffer->status = xir_compile_resource_status(
            xr_compile_resources_resize(buffer->context->resources, &memory, capacity));
        if (buffer->status != XR_XIR_OK) return;
        buffer->text = memory;
        buffer->capacity = capacity;
    }
    if (!emit_work(buffer, 1)) return;
    buffer->text[buffer->length++] = byte;
    (void) emit_label_byte(buffer, byte);
}
/* Until this single final write, callers borrow only the initialized length.
 * Measuring emits no storage and never pays for a fictitious terminator. */
static bool emit_finalize(CBuffer *buffer) {
    if (buffer->status != XR_XIR_OK) return false;
    if (buffer->measuring || !buffer->text || buffer->length >= buffer->capacity || buffer->length >= buffer->limit) {
        buffer->status = XR_XIR_BAD_STRUCTURE;
        return false;
    }
    if (!emit_work(buffer, 1)) return false;
    buffer->text[buffer->length] = 0;
    return true;
}
static bool emit_format_read(CBuffer *buffer, const char **cursor, char *byte) {
    if (!emit_work(buffer, 1)) return false;
    *byte = *(*cursor)++;
    return true;
}
static void emit_text(CBuffer *buffer, const char *text) {
    if (!text) { buffer->status = XR_XIR_BAD_STRUCTURE; return; }
    for (;;) {
        char byte;
        if (!emit_format_read(buffer, &text, &byte) || !byte) return;
        emit_byte(buffer, byte);
    }
}
static void emit_unsigned(CBuffer *buffer, uint64_t value, unsigned base, unsigned width) {
    /* Without label tracking, sizing needs the digit count, not its spelling.
     * Charge the actual divisions and one length update; writing still encodes
     * and copies every output byte through the ordinary path below. */
    if (buffer->measuring && !buffer->tracking) {
        if (buffer->status != XR_XIR_OK) return;
        unsigned count = 0;
        do {
            if (!emit_work(buffer, 1)) return;
            ++count;
            value /= base;
        } while (value);
        size_t emitted = width > count ? width : count;
        if (buffer->length >= buffer->limit || emitted >= buffer->limit - buffer->length) {
            buffer->status = XR_XIR_BUDGET;
            return;
        }
        if (!emit_work(buffer, 1)) return;
        buffer->length += emitted;
        return;
    }
    char digits[20];
    unsigned count = 0;
    do {
        if (!emit_work(buffer, 1)) return;
        digits[count++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value);
    while (width > count) {
        emit_byte(buffer, '0');
        if (buffer->status != XR_XIR_OK) return;
        --width;
    }
    while (count) {
        if (!emit_work(buffer, 1)) return;
        emit_byte(buffer, digits[--count]);
    }
}
static void append(CBuffer *buffer, const char *format, ...) {
    if (buffer->status != XR_XIR_OK) return;
    if (!format) { buffer->status = XR_XIR_BAD_STRUCTURE; return; }
    va_list args;
    va_start(args, format);
    for (;;) {
        char byte;
        if (!emit_format_read(buffer, &format, &byte) || !byte) break;
        if (byte != '%') { emit_byte(buffer, byte); continue; }
        if (!emit_format_read(buffer, &format, &byte)) break;
        if (byte == 's') emit_text(buffer, va_arg(args, const char *));
        else if (byte == 'u') emit_unsigned(buffer, va_arg(args, unsigned int), 10, 0);
        else if (byte == 'd') {
            int value = va_arg(args, int);
            if (value < 0) emit_byte(buffer, '-');
            uint64_t magnitude = value < 0 ? (uint64_t)(-(int64_t)value) : (uint64_t)value;
            emit_unsigned(buffer, magnitude, 10, 0);
        } else if (byte == 'l') {
            if (!emit_format_read(buffer, &format, &byte)) break;
            if (byte != 'l') { buffer->status = XR_XIR_BAD_STRUCTURE; break; }
            if (!emit_format_read(buffer, &format, &byte)) break;
            if (byte != 'u') { buffer->status = XR_XIR_BAD_STRUCTURE; break; }
            emit_unsigned(buffer, va_arg(args, unsigned long long), 10, 0);
        } else if (byte == '0') {
            if (!emit_format_read(buffer, &format, &byte)) break;
            if (byte != '2') { buffer->status = XR_XIR_BAD_STRUCTURE; break; }
            if (!emit_format_read(buffer, &format, &byte)) break;
            if (byte != 'x') { buffer->status = XR_XIR_BAD_STRUCTURE; break; }
            emit_unsigned(buffer, va_arg(args, unsigned int), 16, 2);
        } else if (byte == '%') emit_byte(buffer, '%');
        else { buffer->status = XR_XIR_BAD_STRUCTURE; break; }
    }
    va_end(args);
}
