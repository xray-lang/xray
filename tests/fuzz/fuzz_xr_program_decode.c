/*
 * Task 296 XrProgram structural decoder fuzz entry.
 */

#include "program/xr_program.h"
#include "program/xr_program_decode.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > (1u << 20))
        return 0;
    XrProgramDecodeBudget budget = {
        .max_bytes = 1u << 20,
        .max_records = 1u << 20,
        .max_operations = 1u << 20,
    };
    XrProgramView view;
    XrProgramDecodeStatus status = xr_program_decode_structure(data, size, &budget, &view, NULL, 0);
    if (status == XR_PROGRAM_DECODE_OK) {
        XrProgramArtifact encoded = {0};
        if (xr_program_reencode(&view, &encoded, NULL, 0) != XR_PROGRAM_DECODE_OK ||
            encoded.size != size || memcmp(encoded.bytes, data, size) != 0 ||
            !xr_program_id_equal(encoded.id, view.id))
            abort();
        xr_program_artifact_free(&encoded);
    }
    return 0;
}

#ifdef FUZZ_STANDALONE
int main(void) {
    uint8_t bytes[512];
    uint32_t state = UINT32_C(0x296c0de1);
    for (size_t length = 0; length <= sizeof(bytes); ++length) {
        for (size_t index = 0; index < length; ++index) {
            state = state * UINT32_C(1664525) + UINT32_C(1013904223);
            bytes[index] = (uint8_t) (state >> 24u);
        }
        if (LLVMFuzzerTestOneInput(bytes, length) != 0)
            return 1;
    }
    return 0;
}
#endif
