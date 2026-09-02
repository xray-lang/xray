/*
 * Task 296/297 XrProgram structural decoder and semantic admission fuzz entry.
 */

#include "program/xr_program.h"
#include "program/xr_program_decode.h"
#include "program/xr_program_verify.h"
#include "program/xr_reference_evaluator.h"

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

        XrProgramVerifyBudget verify_budget = xr_program_verify_default_budget();
        verify_budget.decode = budget;
        verify_budget.max_work = 1u << 22;
        XrValidatedProgram *program = NULL;
        XrProgramVerifyStatus verify =
            xr_program_validate(data, size, &verify_budget, &program, NULL);
        if (verify == XR_PROGRAM_VERIFY_OK) {
            size_t retained_size = 0;
            const uint8_t *retained = xr_validated_program_bytes(program, &retained_size);
            if (!retained || retained_size != size || memcmp(retained, data, size) != 0)
                abort();
            XrReferenceProfile profile = {.pointer_width = 64u};
            XrReferenceBudget eval_budget = {
                .max_steps = 1024u,
                .max_value_cells = 1024u,
                .max_call_depth = 32u,
            };
            (void) xr_reference_evaluate(program, xr_validated_program_entry_function(program),
                                         NULL, 0, &profile, &eval_budget);
            xr_validated_program_free(program);
        } else if (program) {
            abort();
        }
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
