/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * http_multipart.c - HTTP multipart/form-data implementation
 *
 * KEY CONCEPT:
 *   RFC 2046 compliant multipart form data encoding
 */

#include "../../src/base/xmalloc.h"
#include "http_multipart.h"
#include "xray_platform.h"  // xr_random_bytes (CSPRNG)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ========== Internal Functions ========== */

// Generate random boundary string using CSPRNG.
//
// Security: RFC 2046 boundaries must be unpredictable enough that an attacker
// cannot embed a forged "--boundary" line inside multipart body and tear the
// form apart. rand()+srand(time(NULL)) produces identical boundaries within
// a 1-second window and is non-cryptographic; use xr_random_bytes instead
// (arc4random_buf on macOS, getrandom() on Linux, BCryptGenRandom on Win).
//
// Layout: "----XrayFormBoundary" (20 bytes) + 16 random alphanumeric chars
// Alphabet size 62, so entropy ~= 16 * log2(62) ~= 95 bits.
// Modulo bias over 62 on uniform bytes is negligible for this purpose.
static char *generate_boundary(void) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    const size_t charset_len = sizeof(charset) - 1;  // 62
    char *boundary = (char *) xr_malloc(48);
    if (!boundary)
        return NULL;

    strcpy(boundary, "----XrayFormBoundary");

    unsigned char rnd[16];
    xr_random_bytes(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) {
        boundary[20 + i] = charset[rnd[i] % charset_len];
    }
    boundary[36] = '\0';

    return boundary;
}

// Guess MIME type
static const char *guess_mime_type(const char *filename) {
    if (!filename)
        return "application/octet-stream";

    const char *ext = strrchr(filename, '.');
    if (!ext)
        return "application/octet-stream";

    ext++;  // Skip dot

    // Common MIME types
    if (strcasecmp(ext, "txt") == 0)
        return "text/plain";
    if (strcasecmp(ext, "html") == 0 || strcasecmp(ext, "htm") == 0)
        return "text/html";
    if (strcasecmp(ext, "css") == 0)
        return "text/css";
    if (strcasecmp(ext, "js") == 0)
        return "application/javascript";
    if (strcasecmp(ext, "json") == 0)
        return "application/json";
    if (strcasecmp(ext, "xml") == 0)
        return "application/xml";
    if (strcasecmp(ext, "pdf") == 0)
        return "application/pdf";
    if (strcasecmp(ext, "zip") == 0)
        return "application/zip";
    if (strcasecmp(ext, "gz") == 0)
        return "application/gzip";
    if (strcasecmp(ext, "tar") == 0)
        return "application/x-tar";
    if (strcasecmp(ext, "png") == 0)
        return "image/png";
    if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0)
        return "image/jpeg";
    if (strcasecmp(ext, "gif") == 0)
        return "image/gif";
    if (strcasecmp(ext, "svg") == 0)
        return "image/svg+xml";
    if (strcasecmp(ext, "ico") == 0)
        return "image/x-icon";
    if (strcasecmp(ext, "mp3") == 0)
        return "audio/mpeg";
    if (strcasecmp(ext, "mp4") == 0)
        return "video/mp4";
    if (strcasecmp(ext, "webm") == 0)
        return "video/webm";

    return "application/octet-stream";
}

/* ========== API Implementation ========== */

XrFormData *xr_form_data_new(void) {
    XrFormData *form = (XrFormData *) xr_calloc(1, sizeof(XrFormData));
    if (form) {
        form->boundary = generate_boundary();
        form->max_total_size = XR_FORM_DATA_DEFAULT_TOTAL_SIZE;
        form->max_file_size = XR_FORM_DATA_DEFAULT_FILE_SIZE;
    }
    return form;
}

void xr_form_data_free(XrFormData *form) {
    if (!form)
        return;

    XrFormField *field = form->fields;
    while (field) {
        XrFormField *next = field->next;
        xr_free(field->name);
        xr_free(field->value);
        xr_free(field->filename);
        xr_free(field->content_type);
        xr_free(field->file_data);
        xr_free(field);
        field = next;
    }

    xr_free(form->boundary);
    xr_free(form);
}

void xr_form_data_append(XrFormData *form, const char *name, const char *value, size_t value_len) {
    if (!form || !name)
        return;

    // Enforce total size limit (0 = no limit)
    if (form->max_total_size > 0 && form->total_size + value_len > form->max_total_size)
        return;

    XrFormField *field = (XrFormField *) xr_calloc(1, sizeof(XrFormField));
    if (!field)
        return;

    field->type = XR_FORM_FIELD_TEXT;
    field->name = xr_strdup(name);

    if (value && value_len > 0) {
        field->value = (char *) xr_malloc(value_len + 1);
        memcpy(field->value, value, value_len);
        field->value[value_len] = '\0';
        field->value_len = value_len;
    }

    // Add to end of list
    if (!form->fields) {
        form->fields = field;
    } else {
        XrFormField *last = form->fields;
        while (last->next)
            last = last->next;
        last->next = field;
    }
    form->total_size += value_len;
    form->field_count++;
}

void xr_form_data_append_file(XrFormData *form, const char *name, const char *filename,
                              const char *content_type, const void *data, size_t size) {
    if (!form || !name || !data || size == 0)
        return;

    // Enforce per-file and total size limits (0 = no limit)
    if (form->max_file_size > 0 && size > form->max_file_size)
        return;
    if (form->max_total_size > 0 && form->total_size + size > form->max_total_size)
        return;

    XrFormField *field = (XrFormField *) xr_calloc(1, sizeof(XrFormField));
    if (!field)
        return;

    field->type = XR_FORM_FIELD_FILE;
    field->name = xr_strdup(name);
    field->filename = filename ? xr_strdup(filename) : xr_strdup("file");
    field->content_type = xr_strdup(content_type ? content_type : guess_mime_type(filename));

    field->file_data = (char *) xr_malloc(size);
    memcpy(field->file_data, data, size);
    field->file_size = size;

    // Add to end of list
    if (!form->fields) {
        form->fields = field;
    } else {
        XrFormField *last = form->fields;
        while (last->next)
            last = last->next;
        last->next = field;
    }
    form->total_size += size;
    form->field_count++;
}

int xr_form_data_append_file_path(XrFormData *form, const char *name, const char *filepath) {
    if (!form || !name || !filepath)
        return -1;

    // Open file
    FILE *fp = fopen(filepath, "rb");
    if (!fp)
        return -1;

    // Get file size
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (size <= 0) {
        fclose(fp);
        return -1;
    }

    if (form->max_file_size > 0 && (size_t) size > form->max_file_size) {
        fclose(fp);
        return -1;
    }

    if (form->max_total_size > 0 && form->total_size + (size_t) size > form->max_total_size) {
        fclose(fp);
        return -1;
    }

    // Read file content
    char *data = (char *) xr_malloc(size);
    if (!data) {
        fclose(fp);
        return -1;
    }

    if (fread(data, 1, size, fp) != (size_t) size) {
        fclose(fp);
        xr_free(data);
        return -1;
    }
    fclose(fp);

    // Extract filename
    const char *filename = strrchr(filepath, '/');
    if (!filename)
        filename = strrchr(filepath, '\\');
    filename = filename ? filename + 1 : filepath;

    // Add file field
    xr_form_data_append_file(form, name, filename, NULL, data, size);
    xr_free(data);

    return 0;
}

int xr_form_data_build(XrFormData *form, char **out, size_t *out_len, char **content_type) {
    if (!form || !out || !out_len)
        return -1;

    // Calculate buffer size
    size_t buf_size = 1024;
    XrFormField *field = form->fields;
    while (field) {
        buf_size += 256;  // Headers
        if (field->type == XR_FORM_FIELD_TEXT) {
            buf_size += field->value_len;
        } else {
            buf_size += field->file_size;
        }
        field = field->next;
    }

    char *buf = (char *) xr_malloc(buf_size);
    if (!buf)
        return -1;

    char *p = buf;

    // Build each field
    field = form->fields;
    while (field) {
        // Boundary
        p += sprintf(p, "--%s\r\n", form->boundary);

        if (field->type == XR_FORM_FIELD_TEXT) {
            // Text field
            p += sprintf(p, "Content-Disposition: form-data; name=\"%s\"\r\n\r\n", field->name);
            if (field->value && field->value_len > 0) {
                memcpy(p, field->value, field->value_len);
                p += field->value_len;
            }
        } else {
            // File field
            p += sprintf(p, "Content-Disposition: form-data; name=\"%s\"; filename=\"%s\"\r\n",
                         field->name, field->filename);
            p += sprintf(p, "Content-Type: %s\r\n\r\n", field->content_type);

            if (field->file_data && field->file_size > 0) {
                memcpy(p, field->file_data, field->file_size);
                p += field->file_size;
            }
        }

        p += sprintf(p, "\r\n");
        field = field->next;
    }

    // End boundary
    p += sprintf(p, "--%s--\r\n", form->boundary);

    *out = buf;
    *out_len = p - buf;

    // Build Content-Type
    if (content_type) {
        *content_type = xr_form_data_content_type(form);
    }

    return 0;
}

char *xr_form_data_content_type(XrFormData *form) {
    if (!form || !form->boundary)
        return NULL;

    size_t len = 64 + strlen(form->boundary);
    char *ct = (char *) xr_malloc(len);
    if (ct) {
        snprintf(ct, len, "multipart/form-data; boundary=%s", form->boundary);
    }
    return ct;
}
