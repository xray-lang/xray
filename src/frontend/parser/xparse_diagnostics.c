/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_diagnostics.c - Explicit compiler policy for the shared formatter
 */
#include "xparse_internal.h"
#include "../xdiag_fmt.h"

static bool parser_diagnostic_work(void *context, uint64_t amount) {
    return xr_compile_state_work(context, amount) == XR_COMPILE_RESOURCE_OK;
}

static void parser_diagnostic_result(Parser *parser, XrDiagStatus status) {
    if (status == XR_DIAG_BAD_ARGUMENT)
        xr_compile_state_fail(parser->state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
    else if (status == XR_DIAG_IO)
        xr_parser_diagnostic_fail(parser, XR_PARSE_IO);
    else if (status == XR_DIAG_RESOURCE && xr_compile_state_status(parser->state) == XR_COMPILE_RESOURCE_OK)
        xr_compile_state_fail(parser->state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
}

bool xr_parser_format(Parser *parser, char *output, size_t capacity, const char *format, ...) {
    if (!xr_parser_healthy(parser)) return false;
    if (!output || !capacity) {
        xr_compile_state_fail(parser->state, XR_COMPILE_RESOURCE_BAD_ARGUMENT);
        return false;
    }
    XrDiagBuffer buffer = {output, capacity, 0, parser->state, parser_diagnostic_work};
    XrDiagPolicy policy = {&buffer, xr_diag_direct_read, xr_diag_buffer_write,
                           xr_diag_buffer_work, XR_DIAG_OK, false};
    va_list arguments;
    va_start(arguments, format);
    (void) xr_diag_format_v(&policy, format, arguments);
    va_end(arguments);
    parser_diagnostic_result(parser, xr_diag_buffer_finish(&policy, &buffer));
    return xr_parser_healthy(parser);
}

void xr_parser_diagnostic_print(Parser *parser, int level, int code,
    const char *message, int line, int column, int token_length, const char *token_start) {
    if (!xr_parser_healthy(parser)) return;
    XrDiagOutput output = {stderr, parser->state, parser_diagnostic_work};
    XrDiagPolicy policy = {&output, xr_diag_direct_read, xr_diag_output_write,
                           xr_diag_output_work, XR_DIAG_OK, false};
    (void) xr_diag_detect_color(&policy);
    parser_diagnostic_result(parser, xr_diag_print_policy(&policy, (XrDiagLevel) level,
        code, message, parser->source_file, line, column, token_length,
        parser->scanner.source, token_start));
}

void xr_parser_diagnostic_summary(Parser *parser) {
    if (!xr_parser_healthy(parser)) return;
    XrDiagOutput output = {stderr, parser->state, parser_diagnostic_work};
    XrDiagPolicy policy = {&output, xr_diag_direct_read, xr_diag_output_write,
                           xr_diag_output_work, XR_DIAG_OK, false};
    (void) xr_diag_detect_color(&policy);
    parser_diagnostic_result(parser, xr_diag_print_summary_policy(&policy, parser->source_file,
        parser->error_count, 0, parser->error_count >= parser->max_errors));
}
