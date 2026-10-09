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

#include "xxir_emit_literal_tables.inc.c"

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
static void emit_payload_byte(CBuffer *buffer, char byte) {
    if (buffer->status != XR_XIR_OK) return;
    if (buffer->length > buffer->limit || buffer->limit - buffer->length < 2) {
        buffer->status = XR_XIR_BUDGET;
        return;
    }
    if (buffer->measuring) {
        if (!emit_work(buffer, 1)) return;
        ++buffer->length;
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
        if (capacity < required || capacity > buffer->limit) capacity = buffer->limit;
        void *memory = buffer->text;
        buffer->status = xir_compile_resource_status(
            xr_compile_resources_resize(buffer->context->resources, &memory, capacity));
        if (buffer->status != XR_XIR_OK) return;
        buffer->text = memory;
        buffer->capacity = capacity;
    }
    if (!emit_work(buffer, 1)) return;
    buffer->text[buffer->length++] = byte;
}
static void emit_byte(CBuffer *buffer, char byte) {
    emit_payload_byte(buffer, byte);
    if (buffer->status == XR_XIR_OK) (void) emit_label_byte(buffer, byte);
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
/* Compile-time literal lengths need no measuring byte scan. Active label
 * tracking and writing still consume the real byte stream, including joins
 * across adjacent emission calls. A rejected sizing span retains exactly the
 * initialized prefix allowed by the original byte bound. */
static void emit_known_literal(CBuffer *buffer, const char *text, size_t length) {
    if (buffer->status != XR_XIR_OK) return;
    if (buffer->measuring && !buffer->tracking) {
        if (!length) return;
        size_t available = buffer->length < buffer->limit ?
            buffer->limit - buffer->length - 1 : 0;
        size_t advanced = length < available ? length : available;
        if (advanced) {
            if (!emit_work(buffer, 1)) return;
            buffer->length += advanced;
        }
        if (advanced < length) buffer->status = XR_XIR_BUDGET;
        return;
    }
    for (size_t i = 0; i < length && buffer->status == XR_XIR_OK; ++i) {
        if (!emit_work(buffer, 1)) return;
        emit_byte(buffer, text[i]);
    }
}
/* Token concatenation rejects pointers; only reviewed NUL/percent-free
 * literals may use this private primitive. */
#define EMIT_LITERAL(buffer, literal) \
    emit_known_literal((buffer), "" literal, sizeof("" literal) - 1)

typedef struct EmitSpanState {
    const uint8_t *tables[2];
    unsigned match[2];
    bool found[2];
    size_t length;
} EmitSpanState;

/* Each begin operation admits before executing. Failed admission reserves no
 * commit and changes no label fact. Valid begin performs eleven operations. */
static bool emit_span_begin(CBuffer *buffer, const EmitSpan *span, EmitSpanState *state) {
    static const unsigned lengths[2] = {13, 11};
    for (unsigned i = 0; i < 2; ++i) {
        if (!emit_work(buffer, 1)) return false;
        if (!emit_work(buffer, 1)) return false;
        state->tables[i] = span->table[i];
        if (!emit_work(buffer, 1)) return false;
        state->match[i] = buffer->label_match[i];
        if (!emit_work(buffer, 1)) return false;
        state->found[i] = buffer->label_used[i];
        if (!emit_work(buffer, 1)) return false;
        if (!state->tables[i] || state->match[i] >= lengths[i] ||
            (state->found[i] && state->match[i])) {
            buffer->status = XR_XIR_BAD_STRUCTURE;
            return false;
        }
    }
    if (!emit_work(buffer, 1)) return false;
    state->length = buffer->length;
    return emit_work(buffer, 15);
}

/* Fifteen prepaid operations always execute, including prefix0/OOM/BUDGET:
 * length read, bounds decision, two loops, four table reads, two selections,
 * four label stores and one tracking store. No byte scan/allocation occurs. */
static void emit_span_commit(CBuffer *buffer, const EmitSpanState *state, size_t length) {
    static const unsigned lengths[2] = {13, 11};
    size_t current = buffer->length;
    size_t prefix = current - state->length;
    if (current < state->length || prefix > length) {
        if (buffer->status == XR_XIR_OK) buffer->status = XR_XIR_BAD_STRUCTURE;
        prefix = 0;
    }
    unsigned found_mask = 0;
    for (unsigned i = 0; i < 2; ++i) {
        size_t width = length + 1 < lengths[i] ? length + 1 : lengths[i];
        size_t short_prefix = prefix < lengths[i] ? prefix : lengths[i] - 1;
        /* Both prepaid reads execute even when only one result is selected. */
        const volatile uint8_t *table = state->tables[i];
        uint8_t short_result = table[state->match[i] * width + short_prefix];
        uint8_t zero_result = table[lengths[i] * width + prefix];
        uint8_t selected = prefix < lengths[i] || (short_result & 128) ? short_result : zero_result;
        bool found = state->found[i] || ((selected & 128) != 0);
        buffer->label_match[i] = found ? 0 : selected;
        buffer->label_used[i] = found;
        found_mask |= (unsigned)found << i;
    }
    buffer->tracking = found_mask != 3;
}

/* Known measuring spans use a length update, not a hidden read loop. Writing
 * retains every input read/output store and the original resize accounting. */
static void emit_static_span(CBuffer *buffer, const EmitSpan *span) {
    if (buffer->status != XR_XIR_OK) return;
    if (!span) { buffer->status = XR_XIR_BAD_STRUCTURE; return; }
    if (!emit_work(buffer, 1)) return;
    size_t length = span->length;
    if (!emit_work(buffer, 1)) return;
    const char *text = span->text;
    if (!text || length > (size_t)INT_MAX - 1) {
        buffer->status = XR_XIR_BAD_STRUCTURE;
        return;
    }
    EmitSpanState state;
    bool tracked = buffer->tracking;
    if (tracked && !emit_span_begin(buffer, span, &state)) return;
    if (buffer->measuring) {
        size_t available = buffer->length < buffer->limit ? buffer->limit - buffer->length - 1 : 0;
        size_t advanced = length < available ? length : available;
        if (advanced) {
            if (!emit_work(buffer, 1)) goto complete;
            buffer->length += advanced;
        }
        if (advanced < length) buffer->status = XR_XIR_BUDGET;
    } else {
        for (size_t i = 0; i < length; ++i) {
            if (!emit_work(buffer, 1)) goto complete;
            char byte = text[i];
            emit_payload_byte(buffer, byte);
            if (buffer->status != XR_XIR_OK) goto complete;
        }
    }
complete:
    if (tracked) emit_span_commit(buffer, &state, length);
}
#define EMIT_STATIC(buffer, id, literal) \
    ((void)sizeof("" literal), emit_static_span((buffer), &emit_literal_spans[id]))

/* One invocation retains original argument-expression evaluation and va_arg
 * types. Descriptor iteration is finite; numeric/text use their old helpers. */
static void emit_template(CBuffer *buffer, const EmitTemplate *format, ...) {
    if (buffer->status != XR_XIR_OK) return;
    if (!format) { buffer->status = XR_XIR_BAD_STRUCTURE; return; }
    if (!emit_work(buffer, 1)) return;
    const EmitPart *parts = format->parts;
    if (!emit_work(buffer, 1)) return;
    size_t count = format->count;
    if (!parts && count) { buffer->status = XR_XIR_BAD_STRUCTURE; return; }
    va_list args;
    va_start(args, format);
    for (size_t i = 0; i < count && buffer->status == XR_XIR_OK; ++i) {
        if (!emit_work(buffer, 1)) break;
        if (!emit_work(buffer, 1)) break;
        unsigned kind = parts[i].kind;
        if (!emit_work(buffer, 1)) break;
        if (kind > EP_S) { buffer->status = XR_XIR_BAD_STRUCTURE; break; }
        if (!emit_work(buffer, 1)) break;
        if (kind == EP_TEXT) {
            unsigned span = parts[i].span;
            if (span >= EMIT_SPAN_COUNT) { buffer->status = XR_XIR_BAD_STRUCTURE; break; }
            emit_static_span(buffer, &emit_literal_spans[span]);
        } else if (kind == EP_S) emit_text(buffer, va_arg(args, const char *));
        else if (kind == EP_U) emit_unsigned(buffer, va_arg(args, unsigned int), 10, 0);
        else if (kind == EP_LLU) emit_unsigned(buffer, va_arg(args, unsigned long long), 10, 0);
        else if (kind == EP_HEX) emit_unsigned(buffer, va_arg(args, unsigned int), 16, 2);
        else {
            int value = va_arg(args, int);
            if (value < 0) emit_byte(buffer, '-');
            uint64_t magnitude = value < 0 ? (uint64_t)(-(int64_t)value) : (uint64_t)value;
            emit_unsigned(buffer, magnitude, 10, 0);
        }
    }
    va_end(args);
}
#define EMIT_FORMAT(buffer, id, format, ...) \
    ((void)sizeof("" format), emit_template((buffer), &emit_literal_templates[id], __VA_ARGS__))

/* Certified single-conversion shapes execute no runtime format/parser lookup.
 * The original helpers retain every numeric, label, byte and resize charge. */
static void emit_decimal_u(CBuffer *buffer, unsigned int value,
    const char *first, size_t first_length, const char *last, size_t last_length) {
    if (buffer->status != XR_XIR_OK) return;
    emit_known_literal(buffer, first, first_length);
    emit_unsigned(buffer, value, 10, 0);
    emit_known_literal(buffer, last, last_length);
}
static void emit_decimal_ull(CBuffer *buffer, unsigned long long value,
    const char *first, size_t first_length, const char *last, size_t last_length) {
    if (buffer->status != XR_XIR_OK) return;
    emit_known_literal(buffer, first, first_length);
    emit_unsigned(buffer, value, 10, 0);
    emit_known_literal(buffer, last, last_length);
}
#define EMIT_U_SHAPE(buffer, id, format, first, last, value) \
    ((void)sizeof("" format), (void)sizeof(emit_literal_templates[id]), \
     emit_decimal_u((buffer), (value), "" first, sizeof("" first)-1, "" last, sizeof("" last)-1))
#define EMIT_ULL_SHAPE(buffer, id, format, first, last, value) \
    ((void)sizeof("" format), (void)sizeof(emit_literal_templates[id]), \
     emit_decimal_ull((buffer), (value), "" first, sizeof("" first)-1, "" last, sizeof("" last)-1))

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
