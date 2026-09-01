/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_identity.c - Runtime-side canonical XrProgram identity
 */

#include "xr_program_internal.h"
#include "xr_program_schema_gen.h"

#include "../base/xsha256.h"

#include <string.h>

void xr_program_compute_id(const uint8_t *bytes, size_t size, XrProgramId *id_out) {
    static const uint8_t domain[] = XR_PROGRAM_ID_DOMAIN;
    XrSHA256Context context;
    if (!id_out)
        return;
    memset(id_out, 0, sizeof(*id_out));
    if (!bytes && size != 0)
        return;
    xr_sha256_init(&context);
    xr_sha256_update(&context, domain, sizeof(domain));
    xr_sha256_update(&context, bytes, size);
    xr_sha256_final(&context, id_out->bytes);
}

bool xr_program_id_equal(XrProgramId left, XrProgramId right) {
    return memcmp(left.bytes, right.bytes, XR_PROGRAM_DIGEST_SIZE) == 0;
}
