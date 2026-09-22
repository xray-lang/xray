/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_program_writer_fixture.h - Xi writer artifact mutation test adapter
 */

#ifndef XR_PROGRAM_WRITER_FIXTURE_H
#define XR_PROGRAM_WRITER_FIXTURE_H

#include "program/xr_program_from_xi.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Mutation tests keep only the untrusted artifact. First check the writer's
 * owned admission result, then release it before testing external admission. */
static XrProgramBuildStatus xr_test_write_program_artifact(const XrProgramFromXiInput *input,
                                                           XrProgramArtifact *artifact,
                                                           char *diagnostic,
                                                           size_t diagnostic_size) {
    XrValidatedProgram *program = NULL;
    XrProgramBuildStatus status =
        xr_program_write_from_xi(input, artifact, &program, NULL, diagnostic, diagnostic_size);
    size_t size = 0u;
    const uint8_t *bytes = xr_validated_program_bytes(program, &size);
    bool valid = status == XR_PROGRAM_BUILD_OK
                     ? program && bytes && bytes != artifact->bytes && size == artifact->size &&
                           memcmp(bytes, artifact->bytes, size) == 0 &&
                           xr_program_id_equal(xr_validated_program_id(program), artifact->id)
                     : !program && !artifact->bytes;
    if (!valid) {
        fprintf(stderr, "Xi writer did not transfer an independent validated product\n");
        abort();
    }
    xr_validated_program_free(program);
    return status;
}

#endif  // XR_PROGRAM_WRITER_FIXTURE_H
