/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_http_multipart.c - Unit tests for HTTP multipart/form-data
 *
 * KEY CONCEPT:
 *   Tests form data creation, field appending, file appending,
 *   body building, and content type generation.
 */

#include "../test_framework.h"
#include <string.h>
#include <stdlib.h>

#include "../../../stdlib/http/http_multipart.h"

/* ========== Create / Free ========== */

TEST(form_data_new_free) {
    XrFormData *form = xr_form_data_new();
    ASSERT_NOT_NULL(form);
    ASSERT_EQ_INT(form->field_count, 0);
    ASSERT_NOT_NULL(form->boundary);
    xr_form_data_free(form);
}

/* ========== Append Text Field ========== */

TEST(form_data_append_text) {
    XrFormData *form = xr_form_data_new();
    xr_form_data_append(form, "name", "John Doe", 8);
    ASSERT_EQ_INT(form->field_count, 1);
    ASSERT_NOT_NULL(form->fields);
    ASSERT_EQ_INT(form->fields->type, XR_FORM_FIELD_TEXT);
    ASSERT_STR_EQ(form->fields->name, "name");
    ASSERT_EQ_INT((int)form->fields->value_len, 8);
    xr_form_data_free(form);
}

TEST(form_data_append_multiple_texts) {
    XrFormData *form = xr_form_data_new();
    xr_form_data_append(form, "first", "Alice", 5);
    xr_form_data_append(form, "last", "Smith", 5);
    xr_form_data_append(form, "age", "30", 2);
    ASSERT_EQ_INT(form->field_count, 3);
    xr_form_data_free(form);
}

/* ========== Append File Field ========== */

TEST(form_data_append_file) {
    XrFormData *form = xr_form_data_new();
    const char *data = "file content here";
    xr_form_data_append_file(form, "upload", "test.txt",
                              "text/plain", data, strlen(data));
    ASSERT_EQ_INT(form->field_count, 1);
    ASSERT_NOT_NULL(form->fields);
    ASSERT_EQ_INT(form->fields->type, XR_FORM_FIELD_FILE);
    ASSERT_STR_EQ(form->fields->name, "upload");
    ASSERT_STR_EQ(form->fields->filename, "test.txt");
    ASSERT_EQ_INT((int)form->fields->file_size, (int)strlen(data));
    xr_form_data_free(form);
}

/* ========== Build Body ========== */

TEST(form_data_build) {
    XrFormData *form = xr_form_data_new();
    xr_form_data_append(form, "key", "value", 5);

    char *body = NULL;
    size_t body_len = 0;
    char *content_type = NULL;

    int ret = xr_form_data_build(form, &body, &body_len, &content_type);
    ASSERT_EQ_INT(ret, 0);
    ASSERT_NOT_NULL(body);
    ASSERT_GT((int)body_len, 0);
    ASSERT_NOT_NULL(content_type);

    // Content-Type should contain "multipart/form-data" and boundary
    ASSERT_NOT_NULL(strstr(content_type, "multipart/form-data"));
    ASSERT_NOT_NULL(strstr(content_type, "boundary="));

    // Body should contain the field
    ASSERT_NOT_NULL(strstr(body, "key"));
    ASSERT_NOT_NULL(strstr(body, "value"));

    free(body);
    free(content_type);
    xr_form_data_free(form);
}

TEST(form_data_build_with_file) {
    XrFormData *form = xr_form_data_new();
    xr_form_data_append(form, "desc", "A file", 6);
    xr_form_data_append_file(form, "file", "data.bin",
                              "application/octet-stream",
                              "\x01\x02\x03", 3);

    char *body = NULL;
    size_t body_len = 0;
    char *content_type = NULL;

    int ret = xr_form_data_build(form, &body, &body_len, &content_type);
    ASSERT_EQ_INT(ret, 0);
    ASSERT_NOT_NULL(body);
    ASSERT_NOT_NULL(content_type);

    // Should contain both fields
    ASSERT_NOT_NULL(strstr(body, "desc"));
    ASSERT_NOT_NULL(strstr(body, "data.bin"));

    free(body);
    free(content_type);
    xr_form_data_free(form);
}

/* ========== Content Type ========== */

TEST(form_data_content_type) {
    XrFormData *form = xr_form_data_new();
    char *ct = xr_form_data_content_type(form);
    ASSERT_NOT_NULL(ct);
    ASSERT_NOT_NULL(strstr(ct, "multipart/form-data"));
    ASSERT_NOT_NULL(strstr(ct, form->boundary));
    free(ct);
    xr_form_data_free(form);
}

/* ========== Empty Form ========== */

TEST(form_data_build_empty) {
    XrFormData *form = xr_form_data_new();

    char *body = NULL;
    size_t body_len = 0;
    char *content_type = NULL;

    int ret = xr_form_data_build(form, &body, &body_len, &content_type);
    ASSERT_EQ_INT(ret, 0);
    ASSERT_NOT_NULL(body);
    ASSERT_NOT_NULL(content_type);

    free(body);
    free(content_type);
    xr_form_data_free(form);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

    RUN_TEST_SUITE("FormData - Create/Free");
    RUN_TEST(form_data_new_free);

    RUN_TEST_SUITE("FormData - Text Fields");
    RUN_TEST(form_data_append_text);
    RUN_TEST(form_data_append_multiple_texts);

    RUN_TEST_SUITE("FormData - File Fields");
    RUN_TEST(form_data_append_file);

    RUN_TEST_SUITE("FormData - Build");
    RUN_TEST(form_data_build);
    RUN_TEST(form_data_build_with_file);
    RUN_TEST(form_data_build_empty);

    RUN_TEST_SUITE("FormData - Content Type");
    RUN_TEST(form_data_content_type);

TEST_MAIN_END()
