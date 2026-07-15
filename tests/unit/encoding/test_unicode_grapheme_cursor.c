/* Unicode 17 UAX #29 extended grapheme cursor conformance tests. */

#include "../test_framework.h"
#include "base/xutf8.h"
#include "shared/xr_unicode_grapheme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef XR_GRAPHEME_BREAK_TEST_FILE
#error "XR_GRAPHEME_BREAK_TEST_FILE must name the vendored official corpus"
#endif

typedef struct TraceEntry {
    size_t offset;
    bool is_break;
    XrGraphemeRule rule;
} TraceEntry;

typedef struct TraceLog {
    TraceEntry entries[128];
    size_t count;
} TraceLog;

static void record_trace(size_t offset, bool is_break, XrGraphemeRule rule, void *context) {
    TraceLog *log = (TraceLog *) context;
    if (log->count < sizeof(log->entries) / sizeof(log->entries[0])) {
        log->entries[log->count++] = (TraceEntry) {offset, is_break, rule};
    }
}

static bool parse_official_case(char *line, uint8_t *bytes, size_t *byte_count, size_t *boundaries,
                                size_t *boundary_count) {
    char *cursor = line;
    char *comment = strchr(line, '#');
    bool saw_token = false;
    *byte_count = 0;
    *boundary_count = 0;
    if (comment)
        *comment = '\0';

    while (*cursor) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')
            cursor++;
        if (!*cursor)
            break;
        if ((unsigned char) cursor[0] == 0xc3 && (unsigned char) cursor[1] == 0xb7) {
            if (*boundary_count >= 128)
                return false;
            boundaries[(*boundary_count)++] = *byte_count;
            cursor += 2;
            saw_token = true;
            continue;
        }
        if ((unsigned char) cursor[0] == 0xc3 && (unsigned char) cursor[1] == 0x97) {
            cursor += 2;
            saw_token = true;
            continue;
        }
        {
            char *end = NULL;
            unsigned long code_point = strtoul(cursor, &end, 16);
            char encoded[XR_UTF8_MAX_BYTES];
            int count;
            if (end == cursor || code_point > 0x10ffffUL)
                return false;
            count = xr_utf8_encode((uint32_t) code_point, encoded);
            if (count <= 0 || *byte_count + (size_t) count > 4096)
                return false;
            memcpy(bytes + *byte_count, encoded, (size_t) count);
            *byte_count += (size_t) count;
            cursor = end;
            saw_token = true;
        }
    }
    return saw_token;
}

static bool collect_boundaries(const uint8_t *bytes, size_t byte_count, size_t *boundaries,
                               size_t *boundary_count, TraceLog *trace) {
    XrGraphemeCursor cursor;
    XrByteRange range;
    *boundary_count = 1;
    boundaries[0] = 0;
    xr_grapheme_cursor_init(&cursor, bytes, byte_count);
    while (xr_grapheme_cursor_next_traced(&cursor, &range, record_trace, trace)) {
        if (*boundary_count >= 128 || range.start != boundaries[*boundary_count - 1] ||
            range.end <= range.start || range.end > byte_count)
            return false;
        boundaries[(*boundary_count)++] = range.end;
    }
    return boundaries[*boundary_count - 1] == byte_count;
}

static void print_trace(size_t line_no, const size_t *expected, size_t expected_count,
                        const size_t *actual, size_t actual_count, const TraceLog *trace) {
    fprintf(stderr, "GraphemeBreakTest line %zu mismatch\n  expected:", line_no);
    for (size_t i = 0; i < expected_count; i++)
        fprintf(stderr, " %zu", expected[i]);
    fprintf(stderr, "\n  actual:");
    for (size_t i = 0; i < actual_count; i++)
        fprintf(stderr, " %zu", actual[i]);
    fprintf(stderr, "\n  decisions:");
    for (size_t i = 0; i < trace->count; i++) {
        fprintf(stderr, " %zu:%s:%s", trace->entries[i].offset,
                trace->entries[i].is_break ? "break" : "keep",
                xr_grapheme_rule_name(trace->entries[i].rule));
    }
    fprintf(stderr, "\n");
}

TEST(grapheme_cursor_empty) {
    XrGraphemeCursor cursor;
    XrByteRange range;
    xr_grapheme_cursor_init(&cursor, NULL, 0);
    ASSERT_FALSE(xr_grapheme_cursor_next(&cursor, &range));
}

TEST(grapheme_cursor_regression_matrix) {
    static const struct {
        const char *bytes;
        size_t byte_count;
        size_t boundaries[5];
        size_t boundary_count;
    } cases[] = {
        {"abc", 3, {0, 1, 2, 3}, 4},
        {"\r\n", 2, {0, 2}, 2},
        {"a\xcc\x88"
         "b",
         4,
         {0, 3, 4},
         3},
        {"\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb", 11, {0, 11}, 2},
        {"\xf0\x9f\x87\xba\xf0\x9f\x87\xb8\xf0\x9f\x87\xa6", 12, {0, 8, 12}, 3},
        {"\xe1\x84\x80\xe1\x85\xa1\xe1\x86\xa8", 9, {0, 9}, 2},
        {"\xe0\xa4\x95\xe0\xa5\x8d\xe0\xa4\x95", 9, {0, 9}, 2},
    };

    for (size_t case_index = 0; case_index < sizeof(cases) / sizeof(cases[0]); case_index++) {
        size_t actual[128];
        size_t actual_count = 0;
        TraceLog trace = {0};
        ASSERT_TRUE(collect_boundaries((const uint8_t *) cases[case_index].bytes,
                                       cases[case_index].byte_count, actual, &actual_count,
                                       &trace));
        ASSERT_EQ_UINT(actual_count, cases[case_index].boundary_count);
        for (size_t i = 0; i < actual_count; i++)
            ASSERT_EQ_UINT(actual[i], cases[case_index].boundaries[i]);
    }
}

TEST(grapheme_cursor_official_conformance) {
    FILE *stream = fopen(XR_GRAPHEME_BREAK_TEST_FILE, "rb");
    char line[8192];
    size_t line_no = 0;
    size_t case_count = 0;
    ASSERT_NOT_NULL(stream);

    while (fgets(line, sizeof(line), stream)) {
        uint8_t bytes[4096];
        size_t expected[128];
        size_t actual[128];
        size_t byte_count;
        size_t expected_count;
        size_t actual_count = 0;
        TraceLog trace = {0};
        line_no++;
        if (!parse_official_case(line, bytes, &byte_count, expected, &expected_count))
            continue;
        case_count++;
        if (!collect_boundaries(bytes, byte_count, actual, &actual_count, &trace) ||
            actual_count != expected_count ||
            memcmp(actual, expected, expected_count * sizeof(expected[0])) != 0) {
            print_trace(line_no, expected, expected_count, actual, actual_count, &trace);
            fclose(stream);
            ASSERT_TRUE(false);
        }
    }
    fclose(stream);
    ASSERT_EQ_UINT(case_count, 766);
}

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Unicode Grapheme - Cursor");
RUN_TEST(grapheme_cursor_empty);
RUN_TEST(grapheme_cursor_regression_matrix);
RUN_TEST(grapheme_cursor_official_conformance);

TEST_MAIN_END()
