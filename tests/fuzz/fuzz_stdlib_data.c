/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * fuzz_stdlib_data.c - Fuzzing harness for pure-Xray data parsers
 *
 * The first input byte selects the stdlib parser:
 *   0: csv.parseReport
 *   1: toml.parseReport
 *   2: xml.parseReport
 *   3: yaml.parseReport
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xray_vm.h"

#define XR_STDLIB_DATA_FUZZ_MAX_INPUT 4096
#define XR_STDLIB_DATA_FUZZ_DEADLINE_MS 2000

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} FuzzBuf;

static void buf_destroy(FuzzBuf *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->cap = 0;
}

static bool buf_reserve(FuzzBuf *buf, size_t extra) {
    if (extra > SIZE_MAX - buf->len - 1)
        return false;
    size_t need = buf->len + extra + 1;
    if (need <= buf->cap)
        return true;

    size_t next = buf->cap ? buf->cap : 256;
    while (next < need) {
        if (next > SIZE_MAX / 2) {
            next = need;
            break;
        }
        next *= 2;
    }

    char *grown = (char *) realloc(buf->data, next);
    if (!grown)
        return false;
    buf->data = grown;
    buf->cap = next;
    return true;
}

static bool buf_append_n(FuzzBuf *buf, const char *s, size_t len) {
    if (!buf_reserve(buf, len))
        return false;
    memcpy(buf->data + buf->len, s, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
    return true;
}

static bool buf_append(FuzzBuf *buf, const char *s) {
    return buf_append_n(buf, s, strlen(s));
}

static bool buf_append_c(FuzzBuf *buf, char c) {
    return buf_append_n(buf, &c, 1);
}

static bool append_literal_char(FuzzBuf *buf, uint8_t byte) {
    switch (byte) {
        case '\n':
            return buf_append(buf, "\\n");
        case '\r':
            return buf_append(buf, "\\r");
        case '\t':
            return buf_append(buf, "\\t");
        case '\\':
            return buf_append(buf, "\\\\");
        case '"':
            return buf_append(buf, "\\\"");
        case '$':
            return buf_append(buf, "\\$");
        case '`':
            return buf_append(buf, "\\`");
        default:
            break;
    }

    if (byte >= 0x20 && byte <= 0x7e)
        return buf_append_c(buf, (char) byte);

    uint8_t mapped = (uint8_t) (0x20 + (byte % 0x5f));
    return append_literal_char(buf, mapped);
}

static bool append_xray_string_literal(FuzzBuf *buf, const uint8_t *data, size_t size) {
    if (!buf_append_c(buf, '"'))
        return false;
    for (size_t i = 0; i < size; i++) {
        if (!append_literal_char(buf, data[i]))
            return false;
    }
    return buf_append_c(buf, '"');
}

static const char *target_prefix(uint8_t selector) {
    switch (selector % 4) {
        case 0:
            return "import csv\nvar data = ";
        case 1:
            return "import toml\nvar data = ";
        case 2:
            return "import xml\nvar data = ";
        default:
            return "import yaml\nvar data = ";
    }
}

static const char *target_suffix(uint8_t selector) {
    switch (selector % 4) {
        case 0:
            return "\nvar options = csv.defaultParseOptions()\n"
                   "options.maxRecords = 2048\n"
                   "var result = csv.parseReport(data, options)\n";
        case 1:
            return "\nvar result = toml.parseReport(data)\n";
        case 2:
            return "\nvar result = xml.parseReport(data)\n";
        default:
            return "\nvar result = yaml.parseReport(data)\n";
    }
}

static char *build_source(const uint8_t *data, size_t size) {
    if (size == 0)
        return NULL;

    FuzzBuf buf = {0};
    uint8_t selector = data[0];
    const uint8_t *payload = data + 1;
    size_t payload_size = size - 1;

    bool ok = buf_append(&buf, target_prefix(selector)) &&
              append_xray_string_literal(&buf, payload, payload_size) &&
              buf_append(&buf, target_suffix(selector));
    if (!ok) {
        buf_destroy(&buf);
        return NULL;
    }
    return buf.data;
}

static int run_source(const char *source) {
    XrVMConfig params;
    xray_vm_config_init(&params);
    XrVMRuntime *iso = xray_vm_new_full(&params);
    if (!iso)
        return 0;

    xray_vm_set_deadline_ms(iso, XR_STDLIB_DATA_FUZZ_DEADLINE_MS);
    int rc = xray_vm_dostring(iso, source);
    bool timed_out = xray_vm_timed_out(iso);
    xray_vm_delete(iso);

    return (rc == 0 && !timed_out) ? 0 : 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size <= 1 || size > XR_STDLIB_DATA_FUZZ_MAX_INPUT)
        return 0;

    char *source = build_source(data, size);
    if (!source)
        return 0;

    int failed = run_source(source);
    free(source);

    if (failed) {
#ifdef FUZZ_STANDALONE
        return 1;
#else
        abort();
#endif
    }
    return 0;
}

#ifdef FUZZ_STANDALONE
static uint8_t *read_all(FILE *f, size_t *out_size) {
    size_t cap = 4096;
    size_t len = 0;
    uint8_t *data = (uint8_t *) malloc(cap);
    if (!data)
        return NULL;

    for (;;) {
        if (len == cap) {
            if (cap > SIZE_MAX / 2) {
                free(data);
                return NULL;
            }
            cap *= 2;
            uint8_t *grown = (uint8_t *) realloc(data, cap);
            if (!grown) {
                free(data);
                return NULL;
            }
            data = grown;
        }
        size_t n = fread(data + len, 1, cap - len, f);
        len += n;
        if (n == 0)
            break;
    }

    *out_size = len;
    return data;
}

int main(int argc, char **argv) {
    FILE *f = stdin;
    if (argc > 1) {
        f = fopen(argv[1], "rb");
        if (!f) {
            perror("fopen");
            return 1;
        }
    }

    size_t size = 0;
    uint8_t *data = read_all(f, &size);
    if (f != stdin)
        fclose(f);
    if (!data)
        return 1;

    int result = LLVMFuzzerTestOneInput(data, size);
    free(data);
    if (result == 0)
        printf("Stdlib data parser processed %zu bytes successfully\n", size);
    return result;
}
#endif
