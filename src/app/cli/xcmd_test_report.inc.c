/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_test_report.inc.c - Owned test outcomes and atomic report publication
 */

typedef struct TestReportBytes {
    XrCompileResources *resources;
    char *data;
    size_t length, capacity;
} TestReportBytes;

typedef struct TestReport {
    TestReportBytes json;
    uint64_t start_ns;
    uint32_t files;
    bool failed;
} TestReport;

typedef enum TestReportStatus {
    TEST_REPORT_PENDING, TEST_REPORT_PASSED, TEST_REPORT_FAILED, TEST_REPORT_ERROR, TEST_REPORT_SKIPPED
} TestReportStatus;

typedef struct TestReportCase {
    const char *name;
    size_t name_length;
    TestReportStatus status;
    TestReportBytes cause;
} TestReportCase;

typedef struct TestReportFile {
    TestReport *owner;
    TestReportCase *cases;
    uint32_t tests, next, errors;
    TestReportBytes error;
    XrSourceCompileStatus source_status;
    XrSourceCompileStage source_stage;
} TestReportFile;

static bool test_report_reserve(TestReportBytes *out, size_t length) {
    if (length > SIZE_MAX - out->length) return false;
    size_t need = out->length + length;
    if (need <= out->capacity) return true;
    size_t capacity = out->capacity ? out->capacity : 256;
    while (capacity < need && capacity <= SIZE_MAX / 2) capacity *= 2;
    if (capacity < need) capacity = need;
    if (xr_compile_resources_resize(out->resources, (void **)&out->data, capacity) != XR_COMPILE_RESOURCE_OK)
        return false;
    out->capacity = capacity;
    return true;
}

static int test_report_write(void *context, const void *bytes, size_t length) {
    TestReportBytes *out = context;
    if (!length) return 1;
    if (!test_report_reserve(out, length)) return 0;
    memcpy(out->data + out->length, bytes, length);
    out->length += length;
    return 1;
}

static bool test_report_text(TestReportBytes *out, const char *text) {
    return test_report_write(out, text, strlen(text));
}

static bool test_report_format(TestReportBytes *out, const char *format, ...) XR_PRINTF_FMT(2, 3);
static bool test_report_format_args(TestReportBytes *out, const char *format, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    bool written = length >= 0 && test_report_reserve(out, (size_t)length + 1) &&
        vsnprintf(out->data + out->length, (size_t)length + 1, format, args) == length;
    if (written) out->length += (size_t)length;
    return written;
}

static bool test_report_format(TestReportBytes *out, const char *format, ...) {
    va_list args;
    va_start(args, format);
    bool written = test_report_format_args(out, format, args);
    va_end(args);
    return written;
}

static bool test_report_quote(TestReportBytes *out, const char *text, size_t length) {
    if (!test_report_text(out, "\"")) return false;
    for (size_t i = 0; i < length;) {
        unsigned char byte = (unsigned char)text[i];
        if (byte == '"' || byte == '\\') {
            char escaped[] = {'\\', (char)byte};
            if (!test_report_write(out, escaped, sizeof(escaped))) return false;
            ++i;
        } else if (byte < 32) {
            if (!test_report_format(out, "\\u%04x", (unsigned)byte)) return false;
            ++i;
        } else if (byte < 128) {
            if (!test_report_write(out, text + i, 1)) return false;
            ++i;
        } else {
            XrUtf8Step step = xr_utf8_core_decode_step((const uint8_t *)text + i, length - i);
            if (step.error != XR_UTF8_OK) {
                if (!test_report_text(out, "\\ufffd")) return false;
                ++i;
            } else {
                if (!test_report_write(out, text + i, step.consumed)) return false;
                i += step.consumed;
            }
        }
    }
    return test_report_text(out, "\"");
}

static bool test_report_allocate(TestReport *report) {
    XrCompileResourceLimits limits = xr_source_compile_default_resource_limits();
    return xr_compile_resources_new(&limits, &report->json.resources) == XR_COMPILE_RESOURCE_OK;
}

static bool test_report_open(TestReport *report) {
    if (!test_report_allocate(report)) return false;
    report->start_ns = xr_time_monotonic_ns();
    return test_report_text(&report->json, "{\"schema\":1,\"files\":[");
}

static bool test_report_publish(TestReport *report, const char *path) {
    uint64_t end = xr_time_monotonic_ns();
    uint64_t elapsed = end >= report->start_ns ? (end - report->start_ns) / UINT64_C(1000000) : 0;
    if (report->failed || !test_report_format(&report->json, "],\"duration_ms\":%" PRIu64 "}\n", elapsed)) {
        xr_cli_error("test", "cannot assemble report '%s'", path);
        return false;
    }
    return xr_cli_lir_publish(report->json.resources, path, report->json.data, report->json.length, false)
        == XR_CLI_EXIT_OK;
}

static void test_report_close(TestReport *report) {
    xr_compile_resources_free(report->json.data);
    xr_compile_resources_release(report->json.resources);
}

/* Each worker has serialized its one row while the Source plan was still live. */
static bool test_report_append(TestReport *report, const TestReport *part) {
    if (part->failed || part->files != 1 || !part->json.length ||
        (report->files && !test_report_text(&report->json, ",")) ||
        !test_report_write(&report->json, part->json.data, part->json.length)) return false;
    ++report->files;
    return true;
}

static void test_report_collect(TestReportFile *file, const XrLirTestPlan *plan) {
    XrCompileResources *resources = file->owner->json.resources;
    uint32_t count = xr_lir_test_plan_role_count(plan);
    if (count && xr_compile_resources_calloc(resources, count, sizeof(*file->cases),
            (void **)&file->cases) != XR_COMPILE_RESOURCE_OK) {
        file->owner->failed = true;
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        XrLirTestPlanRole role;
        if (xr_lir_test_plan_role_at(plan, i, &role) != XR_LIR_OK) {
            file->owner->failed = true;
            return;
        }
        if (role.role != XR_XIR_TEST_ROLE_TEST && role.role != XR_XIR_TEST_ROLE_SKIP) continue;
        file->cases[file->tests++] = (TestReportCase){role.name, role.name_length,
            role.function.id ? TEST_REPORT_PENDING : TEST_REPORT_SKIPPED, {.resources = resources}};
    }
}

static TestReportCase *test_report_next(TestReportFile *file) {
    if (!file->owner || file->owner->failed) return NULL;
    while (file->next < file->tests && file->cases[file->next].status != TEST_REPORT_PENDING) ++file->next;
    if (file->next < file->tests) return &file->cases[file->next++];
    file->owner->failed = true;
    return NULL;
}

static void test_report_suite_error(TestReportFile *file, const char *message) {
    if (!file->owner) return;
    ++file->errors;
    if (file->error.length && !test_report_text(&file->error, "\n")) file->owner->failed = true;
    if (!test_report_text(&file->error, message)) file->owner->failed = true;
}

static bool test_report_file_rows(TestReportFile *file, const char *path) {
    unsigned counts[TEST_REPORT_SKIPPED + 1] = {0};
    const char *statuses[] = {NULL, "passed", "failed", "error", "skipped"};
    for (uint32_t i = 0; i < file->tests; ++i)
        ++counts[file->cases[i].status];
    TestReportBytes *out = &file->owner->json;
    if (!test_report_text(out, file->owner->files ? ",{\"path\":" : "{\"path\":") ||
        !test_report_quote(out, path, strlen(path)) ||
        !test_report_format(out, ",\"tests\":%u,\"passed\":%u,\"failed\":%u,\"errors\":%u,"
            "\"timeouts\":0,\"skipped\":%u", file->tests, counts[TEST_REPORT_PASSED],
            counts[TEST_REPORT_FAILED], counts[TEST_REPORT_ERROR] + file->errors, counts[TEST_REPORT_SKIPPED]))
        return false;
    if (file->error.length && (!test_report_text(out, ",\"error\":") ||
        !test_report_quote(out, file->error.data, file->error.length))) return false;
    if (file->source_status != XR_SOURCE_COMPILE_OK) {
        const char *status = xr_source_compile_status_name(file->source_status);
        if (!test_report_text(out, ",\"sourceStatus\":") || !test_report_quote(out, status, strlen(status)) ||
            !test_report_format(out, ",\"sourceStage\":%u", (unsigned)file->source_stage)) return false;
    }
    if (!test_report_text(out, ",\"cases\":[")) return false;
    bool comma = false;
    for (uint32_t i = 0; i < file->tests; ++i) {
        const TestReportCase *row = &file->cases[i];
        if (row->status == TEST_REPORT_PENDING) continue;
        if (!test_report_text(out, comma ? ",{\"name\":" : "{\"name\":") ||
            !test_report_quote(out, row->name, row->name_length) ||
            !test_report_format(out, ",\"status\":\"%s\"", statuses[row->status])) return false;
        if (row->cause.length && (!test_report_text(out, ",\"message\":") ||
            !test_report_quote(out, row->cause.data, row->cause.length))) return false;
        if (!test_report_text(out, "}")) return false;
        comma = true;
    }
    if (!test_report_text(out, "]}")) return false;
    ++file->owner->files;
    return true;
}

static void test_report_file_close(TestReportFile *file, const char *path) {
    if (!file->owner) return;
    if (!file->owner->failed && !test_report_file_rows(file, path)) file->owner->failed = true;
    for (uint32_t i = 0; i < file->tests; ++i)
        xr_compile_resources_free(file->cases[i].cause.data);
    xr_compile_resources_free(file->cases);
    xr_compile_resources_free(file->error.data);
}
