/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_plan.c - 'xray plan' command implementation
 *
 * KEY CONCEPT:
 *   Target plan artifacts are inspected through the same decoder and proved
 *   through the same verifiers the runtime binds them with. The command
 *   decides nothing about a plan on its own: it reports what those
 *   authorities already decide, and it refuses to call a plan verified on
 *   anything less than the complete chain.
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "../../base/xmalloc.h"
#include "../../plan/format/xr_artifact_kind.h"
#include "../../plan/format/xr_xsm_schema.h"
#include "../../plan/format/xr_xtp_schema.h"
#include "../../plan/format/xr_xtp_text.h"
#include "../../plan/semantic/xr_semantic_ids.h"
#include "../../plan/target/xr_target_instruction_verify.h"
#include "../../plan/target/xr_target_plan.h"
#include "../../plan/target/xr_target_verify.h"
#include "../../../include/xray_target_plan_load.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define XR_PLAN_DIAGNOSTIC_SIZE 512
#define XR_PLAN_DEFAULT_CONTEXT_ROWS 3

static XrArtifactProbeResult classify_file_artifact(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file)
        return xr_artifact_probe(path, NULL, 0);
    uint8_t header[XR_ARTIFACT_PROBE_SIZE] = {0};
    size_t header_size = fread(header, 1, sizeof(header), file);
    fclose(file);
    return xr_artifact_probe(path, header, header_size);
}

static bool read_file_bytes(const char *path, size_t max_size, uint8_t **bytes, size_t *size) {
    *bytes = NULL;
    *size = 0;
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return false;
    }
    long end = ftell(file);
    if (end < 0 || (uint64_t) end > max_size ||
        (uint64_t) end > XR_XTP_MAX_RUNTIME_LOAD_PEAK_BYTES / 2u ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    size_t length = (size_t) end;
    uint8_t *storage = (uint8_t *) xr_malloc(length ? length : 1);
    if (!storage) {
        fclose(file);
        return false;
    }
    bool complete = length == 0 || fread(storage, 1, length, file) == length;
    fclose(file);
    if (!complete) {
        xr_free(storage);
        return false;
    }
    *bytes = storage;
    *size = length;
    return true;
}

/* Report the probe verdict for an artifact that is not the expected kind.
 * Reuses the diagnostic codes the runtime artifact boundary already owns. */
static void report_probe_rejection(const char *path, XrArtifactKind expected,
                                   XrArtifactProbeResult probe) {
    const char *expected_name = expected == XR_ARTIFACT_KIND_XSM ? "semantic module" : "TargetPlan";
    switch (probe.status) {
        case XR_ARTIFACT_PROBE_CONFLICT:
            fprintf(stderr,
                    "XR_ARTIFACT_2006: %s: artifact extension conflicts with its canonical magic\n",
                    path);
            return;
        case XR_ARTIFACT_PROBE_NEED_MORE:
            fprintf(stderr, "XR_ARTIFACT_2001: %s: artifact header is a truncated reserved prefix\n",
                    path);
            return;
        case XR_ARTIFACT_PROBE_UNKNOWN_RESERVED:
            fprintf(stderr,
                    "XR_ARTIFACT_2000: %s: artifact uses an unknown or removed reserved identity\n",
                    path);
            return;
        case XR_ARTIFACT_PROBE_MATCH:
        default:
            fprintf(stderr, "XR_ARTIFACT_2000: %s: artifact is not an exact %s artifact\n", path,
                    expected_name);
            return;
    }
}

static bool load_artifact_bytes(const char *path, XrArtifactKind expected, size_t max_size,
                                uint8_t **bytes, size_t *size) {
    /* An unreadable file has no bytes to probe, and probing one anyway would
     * blame its extension for a conflict it never had. */
    FILE *probe_file = fopen(path, "rb");
    if (!probe_file) {
        fprintf(stderr, "XR_ARTIFACT_2001: %s: artifact cannot be opened for reading\n", path);
        return false;
    }
    fclose(probe_file);
    XrArtifactProbeResult probe = classify_file_artifact(path);
    if (probe.status != XR_ARTIFACT_PROBE_MATCH || probe.kind != expected) {
        report_probe_rejection(path, expected, probe);
        return false;
    }
    if (!read_file_bytes(path, max_size, bytes, size)) {
        fprintf(stderr, "XR_ARTIFACT_2001: %s: artifact cannot be read within its byte budget\n",
                path);
        return false;
    }
    return true;
}

/* Decode one candidate. Decoding is itself the bounded structural proof: it
 * refuses any artifact whose geometry, table counts, or resource manifest
 * exceed the frozen budgets. */
static XrXtpCandidate *decode_plan_artifact(const char *path) {
    uint8_t *bytes = NULL;
    size_t size = 0;
    if (!load_artifact_bytes(path, XR_ARTIFACT_KIND_XTP, XR_XTP_MAX_ARTIFACT_SIZE, &bytes, &size))
        return NULL;
    XrXtpCandidate *candidate = NULL;
    char detail[XR_PLAN_DIAGNOSTIC_SIZE] = {0};
    bool decoded = xr_xtp_decode_candidate(bytes, size, &candidate, detail, sizeof(detail));
    xr_free(bytes);
    if (decoded)
        return candidate;
    fprintf(stderr, "%s: %s\n", path,
            detail[0] ? detail : "XR_ARTIFACT_2000: TargetPlan candidate decoding failed");
    return NULL;
}

static int plan_dump(const char *path) {
    XrXtpCandidate *candidate = decode_plan_artifact(path);
    if (!candidate)
        return XR_CLI_EXIT_FAIL;
    bool dumped = xr_xtp_candidate_dump(candidate, stdout);
    xr_xtp_candidate_release(candidate);
    if (!dumped) {
        fprintf(stderr, "XR_ARTIFACT_2000: %s: TargetPlan artifact cannot be rendered\n", path);
        return XR_CLI_EXIT_FAIL;
    }
    return XR_CLI_EXIT_OK;
}

static int plan_diff(const char *left_path, const char *right_path, uint32_t context_rows) {
    XrXtpCandidate *left = decode_plan_artifact(left_path);
    if (!left)
        return XR_CLI_EXIT_FAIL;
    XrXtpCandidate *right = decode_plan_artifact(right_path);
    if (!right) {
        xr_xtp_candidate_release(left);
        return XR_CLI_EXIT_FAIL;
    }
    printf("plan-diff a=%s b=%s\n", left_path, right_path);
    bool identical = false;
    bool compared = xr_xtp_candidate_diff(left, right, context_rows, stdout, &identical);
    xr_xtp_candidate_release(left);
    xr_xtp_candidate_release(right);
    if (!compared) {
        fprintf(stderr, "XR_ARTIFACT_2000: TargetPlan artifacts cannot be compared\n");
        return XR_CLI_EXIT_FAIL;
    }
    return identical ? XR_CLI_EXIT_OK : XR_CLI_EXIT_FAIL;
}

/* Run the complete verification chain. The semantic authority is not
 * optional: a TargetPlan carries the fingerprint of its semantic plan, never
 * the plan itself, so nothing short of an exact XSM can close the chain. */
static int plan_verify(const char *path, const char *semantic_path) {
    if (!semantic_path) {
        fprintf(stderr,
                "XR_ARTIFACT_2007: TargetPlan verification requires explicit semantic and target "
                "authorities; pass --semantic-plan FILE\n");
        return XR_CLI_EXIT_FAIL;
    }
    if (!xr_runtime_artifact_authority_load_available()) {
        fprintf(stderr, "XR_ARTIFACT_2004: exact semantic authority loading is unavailable\n");
        return XR_CLI_EXIT_UNAVAILABLE;
    }

    uint8_t *semantic_bytes = NULL;
    size_t semantic_size = 0;
    if (!load_artifact_bytes(semantic_path, XR_ARTIFACT_KIND_XSM, XR_XSM_MAX_ARTIFACT_SIZE,
                             &semantic_bytes, &semantic_size))
        return XR_CLI_EXIT_FAIL;
    uint8_t *plan_bytes = NULL;
    size_t plan_size = 0;
    if (!load_artifact_bytes(path, XR_ARTIFACT_KIND_XTP, XR_XTP_MAX_ARTIFACT_SIZE, &plan_bytes,
                             &plan_size)) {
        xr_free(semantic_bytes);
        return XR_CLI_EXIT_FAIL;
    }

    char diagnostic[XR_PLAN_DIAGNOSTIC_SIZE] = {0};
    XrRuntimeArtifactAuthority *authority = NULL;
    bool bound = xr_runtime_artifact_authority_load_xsm(semantic_bytes, semantic_size, &authority,
                                                        diagnostic, sizeof(diagnostic));
    xr_free(semantic_bytes);
    if (!bound) {
        xr_free(plan_bytes);
        fprintf(stderr, "%s: %s\n", semantic_path,
                diagnostic[0] ? diagnostic
                              : "XR_ARTIFACT_2000: semantic authority binding failed");
        return XR_CLI_EXIT_FAIL;
    }

    XrTargetPlan *plan = NULL;
    bool loaded = xr_runtime_target_plan_load(plan_bytes, plan_size, authority, &plan, diagnostic,
                                              sizeof(diagnostic));
    xr_free(plan_bytes);
    xr_runtime_artifact_authority_free(authority);
    if (!loaded) {
        fprintf(stderr, "%s: %s\n", path,
                diagnostic[0] ? diagnostic
                              : "XR_ARTIFACT_2000: TargetPlan materialization failed");
        return XR_CLI_EXIT_FAIL;
    }

    /* Independent re-entry into the same verifiers the builder and the
     * runtime use. Materialization already ran them; running them again on
     * the frozen plan proves the artifact stands on its own. */
    char error[XR_PLAN_DIAGNOSTIC_SIZE] = {0};
    bool verified = xr_target_plan_is_verified(plan) &&
                    xr_target_plan_fingerprint_is_intact(plan) &&
                    xr_target_plan_verify(plan, error, sizeof(error)) &&
                    xr_target_instruction_program_verify(plan, error, sizeof(error));
    if (!verified) {
        fprintf(stderr, "%s: %s\n", path,
                error[0] ? error : "XR_TARGET_1000: TargetPlan is not exactly verified");
        xr_target_plan_free(plan);
        return XR_CLI_EXIT_FAIL;
    }

    char fingerprint[XR_FINGERPRINT_BYTES * 2 + 1];
    char semantic_fingerprint[XR_FINGERPRINT_BYTES * 2 + 1];
    xr_fingerprint_hex(xr_target_plan_fingerprint(plan), fingerprint);
    xr_fingerprint_hex(xr_target_plan_semantic_fingerprint(plan), semantic_fingerprint);
    uint32_t function_count = 0;
    uint32_t instruction_count = 0;
    uint32_t call_count = 0;
    uint32_t slot_count = 0;
    (void) xr_target_plan_functions(plan, &function_count);
    (void) xr_target_plan_instructions(plan, &instruction_count);
    (void) xr_target_plan_calls(plan, &call_count);
    (void) xr_target_plan_slots(plan, &slot_count);
    printf("plan-verify verified path=%s\n", path);
    printf("plan-verify schema=%" PRIu32 " family-mask=0x%016" PRIx64 "\n",
           xr_target_plan_schema_version(plan), xr_target_plan_completed_family_mask(plan));
    printf("plan-verify plan-fingerprint=%s semantic-fingerprint=%s\n", fingerprint,
           semantic_fingerprint);
    printf("plan-verify functions=%" PRIu32 " slots=%" PRIu32 " instructions=%" PRIu32
           " calls=%" PRIu32 "\n",
           function_count, slot_count, instruction_count, call_count);
    xr_target_plan_free(plan);
    return XR_CLI_EXIT_OK;
}

static bool plan_context_rows(const XrCliInvocation *inv, uint32_t *out) {
    *out = XR_PLAN_DEFAULT_CONTEXT_ROWS;
    if (!xr_cli_opt_present(&inv->options, "context"))
        return true;
    int requested = xr_cli_opt_int(&inv->options, "context", -1);
    if (requested < 0 || (uint32_t) requested > XR_XTP_TEXT_MAX_CONTEXT_ROWS) {
        xr_cli_error("plan", "--context must be between 0 and %" PRIu32,
                     XR_XTP_TEXT_MAX_CONTEXT_ROWS);
        return false;
    }
    *out = (uint32_t) requested;
    return true;
}

XR_FUNC int cmd_plan(const XrCliInvocation *inv) {
    if (inv->positional_count < 1) {
        xr_cli_error("plan", "missing subcommand");
        return XR_CLI_EXIT_USAGE;
    }
    const char *subcommand = inv->positionals[0];
    int operands = inv->positional_count - 1;
    const char *semantic_path = xr_cli_opt_string(&inv->options, "semantic-plan", NULL);

    if (strcmp(subcommand, "dump") == 0 || strcmp(subcommand, "verify") == 0) {
        if (operands != 1) {
            xr_cli_error("plan", "%s takes exactly one TargetPlan artifact", subcommand);
            return XR_CLI_EXIT_USAGE;
        }
        if (strcmp(subcommand, "dump") == 0) {
            if (semantic_path) {
                xr_cli_error("plan", "--semantic-plan applies only to 'plan verify'");
                return XR_CLI_EXIT_USAGE;
            }
            return plan_dump(inv->positionals[1]);
        }
        return plan_verify(inv->positionals[1], semantic_path);
    }

    if (strcmp(subcommand, "diff") == 0) {
        if (operands != 2) {
            xr_cli_error("plan", "diff takes exactly two TargetPlan artifacts");
            return XR_CLI_EXIT_USAGE;
        }
        if (semantic_path) {
            xr_cli_error("plan", "--semantic-plan applies only to 'plan verify'");
            return XR_CLI_EXIT_USAGE;
        }
        uint32_t context_rows = 0;
        if (!plan_context_rows(inv, &context_rows))
            return XR_CLI_EXIT_USAGE;
        return plan_diff(inv->positionals[1], inv->positionals[2], context_rows);
    }

    xr_cli_error("plan", "unknown subcommand '%s'", subcommand);
    return XR_CLI_EXIT_USAGE;
}
