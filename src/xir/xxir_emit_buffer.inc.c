/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_emit_buffer.inc.c - C emission with charged byte reads and writes
 *
 * KEY CONCEPT:
 *   The small format vocabulary has no unbounded measuring pass or allocator.
 */
typedef struct CBuffer {
    char *text;
    size_t length, capacity, limit;
    XrXirStatus status;
    const XrXirTypes *types;
    const XrXirCompileContext *context;
} CBuffer;

static bool emit_work(CBuffer *buffer, uint64_t units) {
    if (buffer->status != XR_XIR_OK) return false;
    if (xir_compile_work(buffer->context, units)) return true;
    buffer->status = XR_XIR_BUDGET;
    return false;
}
static void emit_byte(CBuffer *buffer, char byte) {
    if (buffer->status != XR_XIR_OK) return;
    if (buffer->length > buffer->limit || buffer->limit - buffer->length < 2) {
        buffer->status = XR_XIR_BUDGET;
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
    if (!emit_work(buffer, 2)) return;
    buffer->text[buffer->length++] = byte;
    buffer->text[buffer->length] = 0;
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
