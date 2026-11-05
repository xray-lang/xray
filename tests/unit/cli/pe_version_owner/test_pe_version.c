/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_pe_version.c - Independent PE fixtures and resource boundaries
 */
#include "app/toolchain/xtc_xir_pe_version.h"
#include "base/xmalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%d: %s\n", __LINE__, #x); exit(1); } } while (0)
_Static_assert(sizeof(XtcXirPeVersion) == 64, "The version result is a self-contained fixed value");


enum { FRAME = 1024, PE = 64, OPT = 88, SECTION = 328, RESOURCE = 512, LEAF = 640, VALUE = 680 };
static void put16(uint8_t *p, size_t at, uint16_t value) { p[at] = (uint8_t)value; p[at + 1] = (uint8_t)(value >> 8); }
static void put32(uint8_t *p, size_t at, uint32_t value) {
    put16(p, at, (uint16_t)value); put16(p, at + 2, (uint16_t)(value >> 16));
}
static void fixture(uint8_t bytes[FRAME]) {
    memset(bytes, 0, FRAME);
    put16(bytes, 0, 0x5a4d); put32(bytes, 60, PE); put32(bytes, PE, 0x4550);
    put16(bytes, PE + 4, 0x8664); put16(bytes, PE + 6, 1); put16(bytes, PE + 20, 240);
    put16(bytes, OPT, 0x20b); put32(bytes, OPT + 108, 16);
    put32(bytes, OPT + 128, 0x1000); put32(bytes, OPT + 132, 512);
    put32(bytes, SECTION + 8, 512); put32(bytes, SECTION + 12, 0x1000);
    put32(bytes, SECTION + 16, 512); put32(bytes, SECTION + 20, RESOURCE);
    put16(bytes, RESOURCE + 14, 1); put32(bytes, RESOURCE + 16, 16); put32(bytes, RESOURCE + 20, 0x80000020);
    put16(bytes, RESOURCE + 32 + 14, 1); put32(bytes, RESOURCE + 48, 1); put32(bytes, RESOURCE + 52, 0x80000040);
    put16(bytes, RESOURCE + 64 + 14, 1); put32(bytes, RESOURCE + 80, 1033); put32(bytes, RESOURCE + 84, 96);
    put32(bytes, RESOURCE + 96, 0x1080); put32(bytes, RESOURCE + 100, 92);
    put16(bytes, LEAF, 92); put16(bytes, LEAF + 2, 52);
    const char key[] = "VS_VERSION_INFO";
    for (unsigned i = 0; i < sizeof(key); ++i) put16(bytes, LEAF + 6 + i * 2, (uint16_t)key[i]);
    const uint32_t words[13] = {0xfeef04bd,0x10000,0x00010002,0x00030004,0x00050006,0x00070008,0x3f,0,4,1,0,0,0};
    for (unsigned i = 0; i < 13; ++i) put32(bytes, VALUE + i * 4, words[i]);
}
static void parse_case(const uint8_t *bytes, size_t length, XtcXirPeVersionStatus expected) {
    XtcXirPeVersion output, saved;
    memset(&output, 0xa5, sizeof(output)); saved = output;
    CHECK(xtc_xir_pe_version_parse(bytes, length, &output) == expected);
    if (expected != XTC_XIR_PE_VERSION_OK) CHECK(!memcmp(&output, &saved, sizeof(output)));
    else {
        CHECK(!strcmp(output.file_text, "1.2.3.4")); CHECK(!strcmp(output.product_text, "5.6.7.8"));
        for (unsigned i = 0; i < 4; ++i) { CHECK(output.file[i] == i + 1); CHECK(output.product[i] == i + 5); }
    }
}
static void reject_cases(void) {
    uint8_t bytes[FRAME];
    for (size_t i = 0; i < FRAME; ++i) { fixture(bytes); parse_case(bytes, i, XTC_XIR_PE_VERSION_TRUNCATED); }
#define BAD16(at, value, status) do { fixture(bytes); put16(bytes, at, value); parse_case(bytes, FRAME, status); } while (0)
#define BAD32(at, value, status) do { fixture(bytes); put32(bytes, at, value); parse_case(bytes, FRAME, status); } while (0)
    BAD16(0, 0, XTC_XIR_PE_VERSION_INVALID);
    BAD32(60, 0xfffffffc, XTC_XIR_PE_VERSION_TRUNCATED);
    BAD32(60, 65, XTC_XIR_PE_VERSION_INVALID);
    BAD32(PE, 0, XTC_XIR_PE_VERSION_INVALID);
    BAD16(PE + 4, 0x14c, XTC_XIR_PE_VERSION_UNSUPPORTED);
    BAD16(OPT, 0x10b, XTC_XIR_PE_VERSION_UNSUPPORTED);
    BAD16(PE + 6, 0, XTC_XIR_PE_VERSION_INVALID);
    BAD16(PE + 20, 1, XTC_XIR_PE_VERSION_INVALID);
    BAD32(OPT + 108, 17, XTC_XIR_PE_VERSION_INVALID);
    BAD32(OPT + 108, 2, XTC_XIR_PE_VERSION_NOT_FOUND);
    BAD32(OPT + 128, 0xfffffff0, XTC_XIR_PE_VERSION_INVALID);
    BAD32(OPT + 132, 0, XTC_XIR_PE_VERSION_INVALID);
    BAD32(SECTION + 12, 0xffffff00, XTC_XIR_PE_VERSION_INVALID);
    BAD32(SECTION + 20, 0xfffffff0, XTC_XIR_PE_VERSION_TRUNCATED);
    BAD32(RESOURCE + 20, 32, XTC_XIR_PE_VERSION_INVALID);
    BAD32(RESOURCE + 20, 0x800001ff, XTC_XIR_PE_VERSION_INVALID);
    BAD32(RESOURCE + 20, 0x80000000, XTC_XIR_PE_VERSION_INVALID);
    BAD16(RESOURCE + 14, 65535, XTC_XIR_PE_VERSION_INVALID);
    BAD32(RESOURCE + 16, 15, XTC_XIR_PE_VERSION_NOT_FOUND);
    BAD32(RESOURCE + 84, 0x80000060, XTC_XIR_PE_VERSION_INVALID);
    BAD32(RESOURCE + 84, 0x7fffffff, XTC_XIR_PE_VERSION_INVALID);
    BAD32(RESOURCE + 96, 0x11fc, XTC_XIR_PE_VERSION_INVALID);
    BAD32(RESOURCE + 108, 1, XTC_XIR_PE_VERSION_INVALID);
    BAD16(LEAF, 91, XTC_XIR_PE_VERSION_INVALID);
    BAD16(LEAF, 93, XTC_XIR_PE_VERSION_INVALID);
    BAD16(LEAF + 2, 51, XTC_XIR_PE_VERSION_INVALID);
    BAD16(LEAF + 4, 1, XTC_XIR_PE_VERSION_INVALID);
    BAD16(LEAF + 6, 'X', XTC_XIR_PE_VERSION_INVALID);
    BAD32(VALUE, 0, XTC_XIR_PE_VERSION_INVALID);
    BAD32(VALUE + 4, 0x20000, XTC_XIR_PE_VERSION_UNSUPPORTED);
    BAD32(VALUE + 28, 0x10, XTC_XIR_PE_VERSION_INVALID);
    fixture(bytes); put32(bytes, OPT + 128, 0); put32(bytes, OPT + 132, 0);
    parse_case(bytes, FRAME, XTC_XIR_PE_VERSION_NOT_FOUND);
    fixture(bytes); put16(bytes, PE + 6, 2); memcpy(bytes + SECTION + 40, bytes + SECTION, 40);
    parse_case(bytes, FRAME, XTC_XIR_PE_VERSION_AMBIGUOUS);
    /* The second mapping starts inside the requested resource interval. */
    put32(bytes, SECTION + 48, 256); put32(bytes, SECTION + 52, 0x1100);
    put32(bytes, SECTION + 56, 256); put32(bytes, SECTION + 60, 768);
    parse_case(bytes, FRAME, XTC_XIR_PE_VERSION_INVALID);
    /* A virtual-only overlapping section cannot supply file-backed data. */
    put32(bytes, SECTION + 56, 0);
    parse_case(bytes, FRAME, XTC_XIR_PE_VERSION_INVALID);
    fixture(bytes); put16(bytes, RESOURCE + 12, 1); put16(bytes, RESOURCE + 14, 0);
    put32(bytes, RESOURCE + 16, 0x800001ff);
    parse_case(bytes, FRAME, XTC_XIR_PE_VERSION_INVALID);
    /* A second language points at an independent, identical fixed value. */
    fixture(bytes); put16(bytes, RESOURCE + 78, 2); put32(bytes, RESOURCE + 88, 2052); put32(bytes, RESOURCE + 92, 112);
    put32(bytes, RESOURCE + 112, 0x1100); put32(bytes, RESOURCE + 116, 92);
    memcpy(bytes + RESOURCE + 256, bytes + LEAF, 92);
    parse_case(bytes, FRAME, XTC_XIR_PE_VERSION_OK);
    put32(bytes, RESOURCE + 256 + 40 + 8, 0x00010009);
    parse_case(bytes, FRAME, XTC_XIR_PE_VERSION_AMBIGUOUS);
    /* Identical version numbers with different fixed flags still conflict. */
    memcpy(bytes + RESOURCE + 256, bytes + LEAF, 92); put32(bytes, RESOURCE + 256 + 40 + 28, 1);
    parse_case(bytes, FRAME, XTC_XIR_PE_VERSION_AMBIGUOUS);
}
static void value_cases(void) {
    uint8_t storage[FRAME + 1], *bytes = storage + 1; fixture(bytes);
    /* Input pointer alignment is irrelevant to byte decoding. */
    parse_case(bytes, FRAME, XTC_XIR_PE_VERSION_OK);
    for (unsigned i = 2; i < 6; ++i) put32(bytes, VALUE + i * 4, UINT32_MAX);
    XtcXirPeVersion output;
    CHECK(xtc_xir_pe_version_parse(bytes, FRAME, &output) == XTC_XIR_PE_VERSION_OK);
    CHECK(!strcmp(output.file_text, "65535.65535.65535.65535") &&
        !strcmp(output.product_text, "65535.65535.65535.65535"));
    for (unsigned i = 2; i < 6; ++i) put32(bytes, VALUE + i * 4, 0);
    CHECK(xtc_xir_pe_version_parse(bytes, FRAME, &output) == XTC_XIR_PE_VERSION_OK);
    CHECK(!strcmp(output.file_text, "0.0.0.0") && !strcmp(output.product_text, "0.0.0.0"));
    CHECK(!strcmp(output.file_text, "0.0.0.0"));
}
static void ownership_cases(void) {
    uint8_t bytes[FRAME], original[FRAME]; fixture(bytes); memcpy(original, bytes, FRAME);
    XtcXirPeVersion output, saved;
    CHECK(xtc_xir_pe_version_parse(bytes, FRAME, &output) == XTC_XIR_PE_VERSION_OK);
    saved = output;
    CHECK(!memcmp(bytes, original, FRAME));
    CHECK(xtc_xir_pe_version_parse(bytes, FRAME, &output) == XTC_XIR_PE_VERSION_OK);
    CHECK(!memcmp(&output, &saved, sizeof(output)));
    memset(bytes, 0, FRAME);
    CHECK(!strcmp(output.file_text, "1.2.3.4") && !strcmp(output.product_text, "5.6.7.8"));
    CHECK(xtc_xir_pe_version_parse(bytes, FRAME, &output) == XTC_XIR_PE_VERSION_INVALID);
    CHECK(!memcmp(&output, &saved, sizeof(output)));
    CHECK(xtc_xir_pe_version_parse(NULL, FRAME, &output) == XTC_XIR_PE_VERSION_BAD_ARGUMENT);
    CHECK(!memcmp(&output, &saved, sizeof(output)));
    CHECK(xtc_xir_pe_version_parse(bytes, FRAME, NULL) == XTC_XIR_PE_VERSION_BAD_ARGUMENT);
}
static XtcXirPeVersionStatus image_case(uint8_t *bytes, size_t length) {
    XtcXirPeImage image, saved; memset(&image, 0xa7, sizeof(image)); saved = image;
    XtcXirPeVersionStatus status = xtc_xir_pe_image_parse(bytes, length, &image);
    if (status != XTC_XIR_PE_VERSION_OK) CHECK(!memcmp(&saved, &image, sizeof(image)));
    else CHECK(image.machine == 0x8664 && image.optional_magic == 0x20b && image.entry_rva == 0x1000 &&
        image.size_of_image == 0x2000 && image.size_of_headers == 512 && image.subsystem == 3 &&
        image.entry_section_characteristics == 0x60000020);
    return status;
}
static void image_fixture(uint8_t *bytes) {
    fixture(bytes);put16(bytes,PE+22,0x22);put32(bytes,OPT+16,0x1000);put32(bytes,OPT+56,0x2000);
    put32(bytes,OPT+60,512);put16(bytes,OPT+68,3);put32(bytes,SECTION+36,0x60000020);
    put32(bytes,OPT+128,0);put32(bytes,OPT+132,0);
}
static void image_cases(void) {
    uint8_t bytes[FRAME]; image_fixture(bytes);
    CHECK(image_case(bytes,FRAME)==XTC_XIR_PE_VERSION_OK);
    XtcXirPeVersion version;
    CHECK(xtc_xir_pe_version_parse(bytes,FRAME,&version)==XTC_XIR_PE_VERSION_NOT_FOUND);
    for(size_t length=0;length<FRAME;++length)CHECK(image_case(bytes,length)!=XTC_XIR_PE_VERSION_OK);
    put32(bytes,OPT+16,0);CHECK(image_case(bytes,FRAME)==XTC_XIR_PE_VERSION_INVALID);
    image_fixture(bytes);put32(bytes,OPT+16,0x2000);CHECK(image_case(bytes,FRAME)==XTC_XIR_PE_VERSION_INVALID);
    image_fixture(bytes);put32(bytes,OPT+60,SECTION);CHECK(image_case(bytes,FRAME)==XTC_XIR_PE_VERSION_INVALID);
    image_fixture(bytes);put32(bytes,OPT+60,FRAME+1);CHECK(image_case(bytes,FRAME)==XTC_XIR_PE_VERSION_INVALID);
    image_fixture(bytes);put32(bytes,SECTION+16,0);CHECK(image_case(bytes,FRAME)==XTC_XIR_PE_VERSION_INVALID);
    image_fixture(bytes);put16(bytes,PE+6,2);memcpy(bytes+SECTION+40,bytes+SECTION,40);
    CHECK(image_case(bytes,FRAME)==XTC_XIR_PE_VERSION_AMBIGUOUS);
    image_fixture(bytes);put16(bytes,PE+4,0x14c);CHECK(image_case(bytes,FRAME)==XTC_XIR_PE_VERSION_UNSUPPORTED);
    image_fixture(bytes);put16(bytes,OPT,0x10b);CHECK(image_case(bytes,FRAME)==XTC_XIR_PE_VERSION_UNSUPPORTED);
    puts("image_without_version truncations/rva/header: PASS");
}
static void complexity_cases(void) {
    enum { SIZE = 32768, LIMITED = 16384, ROOT = 4096, LOCAL_SIZE = 2048, DATA = 1328, FIXED = 1376 };
    uint8_t bytes[SIZE], fixed[92];
    fixture(bytes); memcpy(fixed, bytes + LEAF, sizeof(fixed));
    /* A single language directory is aliased 128 times at the same depth.
     * Its last visit exceeds 2048 / 8 directory entries. One section gives
     * fewer RVA candidates than LIMITED / 40, isolating directory expansion. */
    memset(bytes, 0, sizeof(bytes)); fixture(bytes);
    put32(bytes, SECTION + 8, LOCAL_SIZE); put32(bytes, SECTION + 16, LOCAL_SIZE);
    put32(bytes, SECTION + 20, ROOT); put32(bytes, OPT + 132, LOCAL_SIZE);
    put16(bytes, ROOT + 14, 1); put32(bytes, ROOT + 16, 16); put32(bytes, ROOT + 20, 0x80000040);
    put16(bytes, ROOT + 64 + 14, 128);
    for (unsigned i = 0; i < 128; ++i) {
        put32(bytes, ROOT + 64 + 16 + (size_t)i * 8, i + 1);
        put32(bytes, ROOT + 64 + 20 + (size_t)i * 8, 0x80000500);
    }
    put16(bytes, ROOT + 1280 + 14, 1); put32(bytes, ROOT + 1296, 1033);
    put32(bytes, ROOT + 1300, DATA); put32(bytes, ROOT + DATA, 0x1000 + FIXED);
    put32(bytes, ROOT + DATA + 4, sizeof(fixed)); memcpy(bytes + ROOT + FIXED, fixed, sizeof(fixed));
    parse_case(bytes, LIMITED, XTC_XIR_PE_VERSION_COMPLEXITY_LIMIT);
    /* The same alias DAG succeeds with sufficient declared resource extent;
     * this rejection is a local resource policy, not a format-invalid result. */
    put32(bytes, SECTION + 8, 4096); put32(bytes, SECTION + 16, 4096);
    put32(bytes, OPT + 132, 4096);
    parse_case(bytes, LIMITED, XTC_XIR_PE_VERSION_OK);

    /* Eight distinct leaves, without directory aliasing, require nine RVA
     * lookups over 64 candidates each. LIMITED / 40 permits 409 candidate visits;
     * the directory entry count is only ten, below its independent bound. */
    memset(bytes, 0, sizeof(bytes)); fixture(bytes);
    memset(bytes + SECTION, 0, 64 * 40);
    put16(bytes, PE + 6, 64); put32(bytes, SECTION + 8, 4096);
    put32(bytes, SECTION + 12, 0x1000); put32(bytes, SECTION + 16, 4096);
    put32(bytes, SECTION + 20, ROOT); put32(bytes, OPT + 132, 4096);
    put16(bytes, ROOT + 14, 1); put32(bytes, ROOT + 16, 16); put32(bytes, ROOT + 20, 0x80000020);
    put16(bytes, ROOT + 32 + 14, 1); put32(bytes, ROOT + 48, 1); put32(bytes, ROOT + 52, 0x80000040);
    put16(bytes, ROOT + 64 + 14, 8);
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t data = 160 + i * 16, leaf = 512 + i * 92;
        put32(bytes, ROOT + 80 + (size_t)i * 8, 1033 + i);
        put32(bytes, ROOT + 84 + (size_t)i * 8, data);
        put32(bytes, ROOT + data, 0x1000 + leaf); put32(bytes, ROOT + data + 4, sizeof(fixed));
        memcpy(bytes + ROOT + leaf, fixed, sizeof(fixed));
    }
    parse_case(bytes, LIMITED, XTC_XIR_PE_VERSION_COMPLEXITY_LIMIT);
    /* Extra physical input extent permits the same section/leaf traversal. */
    parse_case(bytes, SIZE, XTC_XIR_PE_VERSION_OK);
    puts("directory alias expansion and section/leaf traversal limits: PASS");
}
static int real_file(const char *path, const char *file, const char *product) {
    FILE *stream = fopen(path, "rb"); CHECK(stream); CHECK(!fseek(stream, 0, SEEK_END));
    long length = ftell(stream); CHECK(length > 0); rewind(stream);
    uint8_t *bytes = xr_malloc((size_t)length); CHECK(bytes);
    CHECK(fread(bytes, 1, (size_t)length, stream) == (size_t)length); CHECK(!fclose(stream));
    XtcXirPeVersion output;
    CHECK(xtc_xir_pe_version_parse(bytes, (size_t)length, &output) == XTC_XIR_PE_VERSION_OK);
    CHECK(!strcmp(file, output.file_text) && !strcmp(product, output.product_text));
    printf("file=%s file_version=%s product_version=%s\n", path, output.file_text, output.product_text);
    xr_free(bytes); return 0;
}
int main(int argc, char **argv) {
    if (argc == 4) return real_file(argv[1], argv[2], argv[3]);
    CHECK(argc == 1); reject_cases(); value_cases(); ownership_cases(); image_cases(); complexity_cases();
    puts("PE fixed version: PASS"); return 0;
}
