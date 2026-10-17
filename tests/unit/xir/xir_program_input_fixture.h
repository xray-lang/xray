/* Real Packet decoding and permission preparation retain an explicit Input owner. */
#ifndef XIR_PROGRAM_INPUT_FIXTURE_H
#define XIR_PROGRAM_INPUT_FIXTURE_H
#include "xir/xxir_program_internal.h"
static XrXirStatus program_fixture_decode_prepare(const XrXirCompileContext *context,
    const XrXirProgramSpec *spec, const XrXirProgramPacket *packet,
    XrXirProgramInput **input, XrXirProgramPermissions **permissions) {
    if (!spec || !input || *input || !permissions || *permissions)
        return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status = xir_program_input_read(context, packet, &spec->target, input);
    if (status == XR_XIR_OK)
        status = xir_program_input_prepare(context, spec, *input, permissions);
    if (status != XR_XIR_OK) {
        xir_program_input_free(*input);
        *input = NULL;
    }
    return status;
}
#endif // XIR_PROGRAM_INPUT_FIXTURE_H
