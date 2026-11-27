/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * lir_test_suite_catalog.h - Private metadata carried by the emitted C
 */
#ifndef LIR_TEST_SUITE_CATALOG_H
#define LIR_TEST_SUITE_CATALOG_H
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define SUITE_SOURCE "demos/07-advanced/testing.xr"

enum { SUITE_TEST = 1, SUITE_SKIP = 2, SUITE_DIGEST_SIZE = 32 };

typedef struct SuiteRow {
    const char *name;
    uint32_t function, role, timeout, action;
} SuiteRow;

typedef struct SuiteCatalog {
    uint8_t source_sha256[SUITE_DIGEST_SIZE];
    const SuiteRow *rows;
    uint32_t count;
} SuiteCatalog;

typedef struct SuiteResults {
    uint32_t passed, skipped, failed;
} SuiteResults;

/* The inventory authorizes no calls. Only the emitted runtime action table
 * is invoked, with a one-to-one correspondence for the runnable rows. */
static inline bool suite_catalog_valid(const SuiteCatalog *catalog, uint32_t actions) {
    if (!catalog || !catalog->rows || !catalog->count || !actions) return false;
    uint32_t next = 0;
    for (uint32_t i = 0; i < catalog->count; ++i) {
        const SuiteRow *row = &catalog->rows[i];
        if (!row->name || !row->name[0] || row->function == UINT32_MAX) return false;
        if (row->role == SUITE_TEST) {
            if (row->timeout || next >= actions || row->action != next++) return false;
        } else if (row->role != SUITE_SKIP || row->action != UINT32_MAX) {
            return false;
        }
        for (uint32_t j = 0; j < i; ++j)
            if (row->function == catalog->rows[j].function || !strcmp(row->name, catalog->rows[j].name))
                return false;
    }
    return next == actions;
}

static inline bool suite_json_string(FILE *out, const char *text) {
    if (fputc('"', out) == EOF) return false;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if ((*p == '"' || *p == '\\') && fputc('\\', out) == EOF) return false;
        if (*p < 32) {
            if (fprintf(out, "\\u%04x", (unsigned)*p) < 0) return false;
        } else if (fputc(*p, out) == EOF) return false;
    }
    return fputc('"', out) != EOF;
}

static inline bool suite_report(FILE *out, const SuiteCatalog *catalog, SuiteResults results) {
    if (fputs("{\"schema\":1,\"source\":\"" SUITE_SOURCE "\",\"sourceSha256\":\"", out) < 0)
        return false;
    for (size_t i = 0; i < SUITE_DIGEST_SIZE; ++i)
        if (fprintf(out, "%02x", (unsigned)catalog->source_sha256[i]) < 0) return false;
    if (fputs("\",\"entries\":[", out) < 0) return false;
    for (uint32_t i = 0; i < catalog->count; ++i) {
        const SuiteRow *row = &catalog->rows[i];
        if (fputs(i ? ",{\"name\":" : "{\"name\":", out) < 0 ||
            !suite_json_string(out, row->name) ||
            fprintf(out, ",\"function\":%" PRIu32 ",\"role\":%" PRIu32 ",\"timeout\":%" PRIu32 "}",
                row->function, row->role, row->timeout) < 0) return false;
    }
    return fprintf(out, "],\"passed\":%" PRIu32 ",\"skipped\":%" PRIu32 ",\"failed\":%" PRIu32 "}\n",
        results.passed, results.skipped, results.failed) >= 0;
}

static inline bool suite_write_report(const char *path, const SuiteCatalog *catalog, SuiteResults results) {
    FILE *output = fopen(path, "wbx");
    if (!output) return false;
    bool written = suite_report(output, catalog, results);
    bool closed = fclose(output) == 0;
    if (written && closed) return true;
    (void)remove(path);
    return false;
}

static inline void suite_record(SuiteResults *results, const char *backend, const SuiteRow *row,
    bool passed) {
    if (row->role == SUITE_SKIP) {
        ++results->skipped;
        return;
    }
    if (passed) ++results->passed;
    else ++results->failed;
    printf("%s %s %s\n", backend, row->name, passed ? "PASS" : "FAIL");
}

static inline void suite_summary(SuiteResults results) {
    printf("original catalog: %" PRIu32 " PASS, %" PRIu32 " SKIP", results.passed, results.skipped);
    if (results.failed) printf(", %" PRIu32 " FAIL", results.failed);
    putchar('\n');
}

#endif // LIR_TEST_SUITE_CATALOG_H
