/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_value_format_arrays.c - Exact shared Array display and traversal boundaries
 */
#include "shared/xr_value_format_core.h"
#include <stdlib.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); exit(1); } } while (0)
typedef struct FormatNode {
    XrValueFormatKind kind;
    int64_t number;
    const char *bytes;
    size_t length;
    const struct FormatNode *children;
    size_t count;
} FormatNode;
typedef struct FormatProbe {
    char bytes[4096];
    size_t used, writes, fail_at, reads, child_reads;
} FormatProbe;

static int format_read(const void *context, XrValueFormatNode node, XrValueFormatView *view) {
    FormatProbe *probe = (FormatProbe *)context;
    const FormatNode *source = node.value;
    ++probe->reads;
    view->kind = source->kind;
    view->signed_value = source->number;
    view->unsigned_value = (uint64_t)source->number;
    view->bytes = (const unsigned char *)source->bytes;
    view->size = source->kind == XR_VALUE_FORMAT_ARRAY ? source->count : source->length;
    view->quote = source->kind == XR_VALUE_FORMAT_BYTES ? '"' : '\0';
    if (source->kind == XR_VALUE_FORMAT_LITERAL) { view->name = source->bytes; view->name_size = source->length; }
    if (source->kind == XR_VALUE_FORMAT_ENUM) {
        view->name = "Color"; view->name_size = 5; view->member = "Green"; view->member_size = 5;
    }
    return 1;
}
static int format_child(const void *context, XrValueFormatNode node, uint32_t index, XrValueFormatNode *child) {
    FormatProbe *probe = (FormatProbe *)context;
    const FormatNode *source = node.value;
    ++probe->child_reads;
    CHECK(index < source->count && index < XR_VALUE_FORMAT_MAX_ELEMENTS && source->children);
    *child = (XrValueFormatNode){&source->children[index], 0};
    return 1;
}
static int format_write(void *context, const void *bytes, size_t length) {
    FormatProbe *probe = context;
    if (probe->writes++ == probe->fail_at) return 0;
    if (length > sizeof(probe->bytes) - probe->used) return 0;
    if (length) memcpy(probe->bytes + probe->used, bytes, length);
    probe->used += length;
    return 1;
}
static int render(const FormatNode *node, FormatProbe *probe) {
    XrValueFormatReader reader = {probe, format_read, format_child};
    return xr_value_format_value(reader, (XrValueFormatNode){node, 0},
        (XrValueFormatSink){probe, format_write}, 0);
}
static void exact(const FormatNode *node, const char *expected, size_t length) {
    FormatProbe probe = {0}; probe.fail_at = SIZE_MAX;
    CHECK(render(node, &probe));
    CHECK(probe.used == length && !memcmp(probe.bytes, expected, length));
}
static void scalar_and_nested(void) {
    const FormatNode leaves[] = {
        {.kind=XR_VALUE_FORMAT_SIGNED, .number=INT64_MIN},
        {.kind=XR_VALUE_FORMAT_UNSIGNED, .number=-1},
        {.kind=XR_VALUE_FORMAT_BOOL, .number=1},
        {.kind=XR_VALUE_FORMAT_BYTES, .bytes="A\0\xE4\xB8\xAD", .length=5},
        {.kind=XR_VALUE_FORMAT_ENUM},
        {.kind=XR_VALUE_FORMAT_LITERAL, .bytes="null", .length=4}
    };
    const FormatNode node = {.kind=XR_VALUE_FORMAT_ARRAY, .children=leaves, .count=6};
    static const char expected[] = "[-9223372036854775808, 18446744073709551615, true, \"A\0\xE4\xB8\xAD\", Color.Green, null]";
    exact(&node, expected, sizeof(expected)-1);
    const FormatNode empty = {.kind=XR_VALUE_FORMAT_ARRAY};
    exact(&empty, "[]", 2);
    FormatProbe probe = {0}; probe.fail_at=SIZE_MAX;
    XrValueFormatReader reader = {&probe,format_read,NULL};
    CHECK(xr_value_format_value(reader,(XrValueFormatNode){&empty,0},
        (XrValueFormatSink){&probe,format_write},0) && probe.used==2 && !probe.child_reads);
    const FormatNode rows[] = {node, empty};
    const FormatNode nested = {.kind=XR_VALUE_FORMAT_ARRAY, .children=rows, .count=2};
    static const char expected_nested[] = "[[-9223372036854775808, 18446744073709551615, true, \"A\0\xE4\xB8\xAD\", Color.Green, null], []]";
    exact(&nested, expected_nested, sizeof(expected_nested)-1);
}
static void traversal_limits(void) {
    FormatNode leaves[33] = {{0}};
    for (size_t i=0; i<33; ++i) leaves[i]=(FormatNode){.kind=XR_VALUE_FORMAT_SIGNED, .number=(int64_t)i};
    FormatNode array = {.kind=XR_VALUE_FORMAT_ARRAY, .children=leaves, .count=33};
    FormatProbe probe = {0}; probe.fail_at=SIZE_MAX;
    CHECK(render(&array, &probe) && probe.reads==33 && probe.child_reads==32);
    static const char expected[] = "[0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, ...(1 more)]";
    CHECK(probe.used==sizeof(expected)-1 && !memcmp(probe.bytes,expected,sizeof(expected)-1));
#if SIZE_MAX >= INT64_MAX
    array.count=(size_t)INT64_MAX; probe=(FormatProbe){0}; probe.fail_at=SIZE_MAX;
    CHECK(render(&array,&probe) && probe.reads==33 && probe.child_reads==32);
    static const char tail[] = ", ...(9223372036854775775 more)]";
    CHECK(probe.used>=sizeof(tail)-1 && !memcmp(probe.bytes+probe.used-(sizeof(tail)-1),tail,sizeof(tail)-1));
#endif
    FormatNode levels[5] = {{0}};
    for (size_t i=0; i<5; ++i) levels[i]=(FormatNode){.kind=XR_VALUE_FORMAT_ARRAY, .children=i<4?&levels[i+1]:NULL, .count=1};
    probe=(FormatProbe){0}; probe.fail_at=SIZE_MAX;
    CHECK(render(&levels[0],&probe) && probe.reads==5 && probe.child_reads==4);
    CHECK(probe.used==11 && !memcmp(probe.bytes,"[[[[...]]]]",11));
}
static void sink_failures(void) {
    const FormatNode leaves[] = {{.kind=XR_VALUE_FORMAT_SIGNED,.number=7},{.kind=XR_VALUE_FORMAT_SIGNED,.number=9}};
    const FormatNode array = {.kind=XR_VALUE_FORMAT_ARRAY,.children=leaves,.count=2};
    FormatProbe baseline = {0}; baseline.fail_at=SIZE_MAX; CHECK(render(&array,&baseline));
    CHECK(baseline.writes>1);
    for (size_t site=0; site<baseline.writes; ++site) {
        FormatProbe probe={0}; probe.fail_at=site;
        CHECK(!render(&array,&probe) && probe.writes==site+1 && probe.used<=baseline.used);
    }
#if SIZE_MAX > INT64_MAX
    const FormatNode impossible = {.kind=XR_VALUE_FORMAT_ARRAY,.count=SIZE_MAX};
    FormatProbe probe={0}; probe.fail_at=SIZE_MAX;
    CHECK(!render(&impossible,&probe) && !probe.writes && !probe.child_reads);
#endif
}
_Static_assert(XR_VALUE_FORMAT_SIGNED == 1 && XR_VALUE_FORMAT_LITERAL == 6,
    "Existing format kinds retain their identities");
int main(void) {
    scalar_and_nested(); traversal_limits(); sink_failures();
    puts("Shared Array display: exact bytes, original depth/element limits and every sink failure passed");
    return 0;
}
