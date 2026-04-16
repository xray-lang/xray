/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_multipart.h - HTTP multipart/form-data support
 *
 * KEY CONCEPT:
 *   Build multipart/form-data request body with file upload
 *   and multiple field support.
 */

#ifndef XR_STDLIB_HTTP_MULTIPART_H
#define XR_STDLIB_HTTP_MULTIPART_H

#include <stddef.h>
#include <stdbool.h>

/* ========== Form Field ========== */

typedef enum {
    XR_FORM_FIELD_TEXT,     // Text field
    XR_FORM_FIELD_FILE      // File field
} XrFormFieldType;

typedef struct XrFormField {
    XrFormFieldType type;
    char *name;             // Field name
    
    // Text field
    char *value;            // Value
    size_t value_len;
    
    // File field
    char *filename;         // Filename
    char *content_type;     // MIME type
    char *file_data;        // File data
    size_t file_size;       // File size
    
    struct XrFormField *next;
} XrFormField;

/* ========== Form Data ========== */

// Default limits (overridable per-instance, no hard cap)
#define XR_FORM_DATA_DEFAULT_TOTAL_SIZE   (64 * 1024 * 1024)   // 64MB total
#define XR_FORM_DATA_DEFAULT_FILE_SIZE    (32 * 1024 * 1024)   // 32MB per file

typedef struct XrFormData {
    XrFormField *fields;    // Field linked list
    int field_count;        // Field count
    char *boundary;         // Boundary string
    size_t total_size;      // Running total of all field data
    size_t max_total_size;  // 0 = no limit
    size_t max_file_size;   // 0 = no limit
} XrFormData;

/* ========== API ========== */

/*
 * Create form data
 */
XrFormData* xr_form_data_new(void);

/*
 * Free form data
 */
void xr_form_data_free(XrFormData *form);

/*
 * Append text field
 */
void xr_form_data_append(XrFormData *form, 
                          const char *name,
                          const char *value,
                          size_t value_len);

/*
 * Append file field
 * 
 * name: Field name
 * filename: Filename (display name)
 * content_type: MIME type (optional, default application/octet-stream)
 * data: File data
 * size: File size
 */
void xr_form_data_append_file(XrFormData *form,
                               const char *name,
                               const char *filename,
                               const char *content_type,
                               const void *data,
                               size_t size);

/*
 * Append file from file path
 * Returns: 0 on success, -1 on failure
 */
int xr_form_data_append_file_path(XrFormData *form,
                                   const char *name,
                                   const char *filepath);

/*
 * Build multipart/form-data request body
 * 
 * out: Output buffer (caller must free)
 * out_len: Output length
 * content_type: Output Content-Type (with boundary, caller must free)
 * 
 * Returns: 0 on success, -1 on failure
 */
int xr_form_data_build(XrFormData *form,
                        char **out,
                        size_t *out_len,
                        char **content_type);

/*
 * Get Content-Type header value (with boundary)
 * Returns: newly allocated string (caller must free)
 */
char* xr_form_data_content_type(XrFormData *form);

#endif
