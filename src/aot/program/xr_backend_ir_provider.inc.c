/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_backend_ir_provider.inc.c - Declared host adapters for native execution
 *
 * KEY CONCEPT:
 *   Generated adapter bodies call the same typed host implementation as VM
 *   bindings. This consumer supplies only dense Program indexes and private
 *   boolean carrier conversion; it never implements host service semantics.
 */

#include "../../runtime/abi/xr_stdlib_provider_contract.h"

typedef enum XrAotNativeProviderKind {
    XR_AOT_NATIVE_I64_NULLARY = 1,
    XR_AOT_NATIVE_I64_UNARY,
    XR_AOT_NATIVE_BOOL_I64_UNARY,
    XR_AOT_NATIVE_OPTIONAL_I64_PAIR_NULLARY,
    XR_AOT_NATIVE_KIND_COUNT,
} XrAotNativeProviderKind;

typedef struct XrAotNativeProviderSource {
    XrStableId contract_id;
    XrStableId operation_id;
    XrAotNativeProviderKind kind;
    const char *header;
    const char *definition;
} XrAotNativeProviderSource;

#include "xr_provider_aot_sources_gen.inc.c"

static const char *native_provider_kind_name(XrAotNativeProviderKind kind) {
    switch (kind) {
        case XR_AOT_NATIVE_I64_NULLARY:
            return "i64_nullary";
        case XR_AOT_NATIVE_I64_UNARY:
            return "i64_unary";
        case XR_AOT_NATIVE_BOOL_I64_UNARY:
            return "bool_i64_unary";
        case XR_AOT_NATIVE_OPTIONAL_I64_PAIR_NULLARY:
            return "optional_i64_pair_nullary";
        default:
            return NULL;
    }
}

static bool native_provider_requirement(const XrBackendIR *ir, size_t index,
                                        uint32_t *requirement_out, uint32_t *operation_out) {
    const XrAotNativeProviderSource *source = &xr_aot_native_provider_sources[index];
    for (uint32_t requirement = 0u;
         requirement < xr_validated_program_provider_requirement_count(ir->program);
         ++requirement) {
        XrProgramProviderRequirementView view = {0};
        if (!xr_validated_program_provider_requirement(ir->program, requirement, &view) ||
            !xr_stable_id_equal(view.contract_id, source->contract_id))
            continue;
        for (uint32_t operation = 0u; operation < view.operation_count; ++operation) {
            const XrProgramProviderOperationRequirement *required = &view.operations[operation];
            if (!xr_stable_id_equal(required->operation_id, source->operation_id))
                continue;
            const XrStdlibProviderDescriptor *descriptor =
                xr_stdlib_provider_find(view.contract_id, required->operation_id);
            XrProviderLogicalContract logical;
            if (!xr_stdlib_provider_logical(descriptor, &logical) ||
                !xr_provider_logical_contract_equal(&logical, &required->logical_contract))
                return false;
            *requirement_out = requirement;
            *operation_out = operation;
            return true;
        }
    }
    return false;
}

static bool native_provider_kind_used(const XrBackendIR *ir, XrAotNativeProviderKind kind) {
    for (size_t index = 0u; index < XR_AOT_NATIVE_PROVIDER_SOURCE_COUNT; ++index) {
        uint32_t requirement, operation;
        if (xr_aot_native_provider_sources[index].kind == kind &&
            native_provider_requirement(ir, index, &requirement, &operation))
            return true;
    }
    return false;
}

static bool native_provider_is_byte_sink(XrStableId contract,
                                         const XrProgramProviderOperationRequirement *operation) {
    XrStableId io, output, assertion;
    XrFingerprint digest;
    XrProviderLogicalContract logical = xr_builtin_provider_byte_sink_logical_contract();
    return xr_stable_id_from_key(XR_PROVIDER_IO_CONTRACT_KEY, &io, &digest) &&
           xr_stable_id_from_key(XR_PROVIDER_IO_OUTPUT_WRITE_OPERATION_KEY, &output, &digest) &&
           xr_stable_id_from_key(XR_PROVIDER_IO_ASSERTION_REPORT_OPERATION_KEY, &assertion,
                                 &digest) &&
           xr_stable_id_equal(contract, io) &&
           (xr_stable_id_equal(operation->operation_id, output) ||
            xr_stable_id_equal(operation->operation_id, assertion)) &&
           xr_provider_logical_contract_equal(&logical, &operation->logical_contract);
}

static bool native_provider_requirements_supported(const XrBackendIR *ir) {
    for (uint32_t requirement = 0u;
         requirement < xr_validated_program_provider_requirement_count(ir->program);
         ++requirement) {
        XrProgramProviderRequirementView view = {0};
        if (!xr_validated_program_provider_requirement(ir->program, requirement, &view))
            return false;
        for (uint32_t operation = 0u; operation < view.operation_count; ++operation) {
            if (native_provider_is_byte_sink(view.contract_id, &view.operations[operation]))
                continue;
            bool found = false;
            for (size_t index = 0u; index < XR_AOT_NATIVE_PROVIDER_SOURCE_COUNT; ++index) {
                uint32_t selected_requirement, selected_operation;
                if (native_provider_requirement(ir, index, &selected_requirement,
                                                &selected_operation) &&
                    selected_requirement == requirement && selected_operation == operation) {
                    found = true;
                    break;
                }
            }
            if (!found)
                return false;
        }
    }
    return true;
}

static bool emit_native_provider_headers(CBuffer *buffer, const XrBackendIR *ir) {
    if (!native_provider_requirements_supported(ir))
        return false;
    if (!append_text(buffer, "#include <stdbool.h>\n"))
        return false;
    for (size_t index = 0u; index < XR_AOT_NATIVE_PROVIDER_SOURCE_COUNT; ++index) {
        uint32_t requirement, operation;
        if (native_provider_requirement(ir, index, &requirement, &operation) &&
            !append_format(buffer, "#include \"%s\"\n",
                           xr_aot_native_provider_sources[index].header))
            return false;
    }
    bool has_native = false;
    for (XrAotNativeProviderKind kind = XR_AOT_NATIVE_I64_NULLARY; kind < XR_AOT_NATIVE_KIND_COUNT;
         ++kind)
        has_native |= native_provider_kind_used(ir, kind);
    if (has_native) {
        const XrTargetMachineFacts *machine = xr_target_profile_machine_facts(ir->profile);
        const char *platform;
        switch (machine->operating_system) {
            case XR_TARGET_OS_LINUX:
                platform = "XR_OS_LINUX";
                break;
            case XR_TARGET_OS_MACOS:
                platform = "XR_OS_MACOS";
                break;
            case XR_TARGET_OS_WINDOWS:
                platform = "XR_OS_WINDOWS";
                break;
            default:
                return false;
        }
        if (!append_format(
                buffer,
                "#if !defined(%s)\n"
                "#error Generated provider target does not match the selected C toolchain\n"
                "#endif\n"
                "_Static_assert(sizeof(void *) == %u && _Alignof(void *) == %u,\n"
                "    \"Generated provider pointer ABI does not match the selected C "
                "toolchain\");\n",
                platform, (unsigned) machine->data_layout.pointer.size,
                (unsigned) machine->data_layout.pointer.align))
            return false;
    }
    return true;
}

static bool emit_native_provider_call(CBuffer *buffer, XrAotNativeProviderKind kind, size_t index) {
    switch (kind) {
        case XR_AOT_NATIVE_I64_NULLARY:
            return append_format(
                buffer, "        return xr_aot_native_provider_%zu(context, result);\n", index);
        case XR_AOT_NATIVE_I64_UNARY:
            return append_format(
                buffer, "        return xr_aot_native_provider_%zu(context, argument, result);\n",
                index);
        case XR_AOT_NATIVE_BOOL_I64_UNARY:
            return append_format(
                buffer,
                "        bool value = false;\n"
                "        int status = xr_aot_native_provider_%zu(context, argument, &value);\n"
                "        if (status == 0) *result = value ? UINT8_C(1) : UINT8_C(0);\n"
                "        return status;\n",
                index);
        case XR_AOT_NATIVE_OPTIONAL_I64_PAIR_NULLARY:
            return append_format(
                buffer,
                "        bool value = false;\n"
                "        int64_t left = 0, right = 0;\n"
                "        int status = xr_aot_native_provider_%zu(context, &value, &left, &right);\n"
                "        if (status == 0) {\n"
                "            *present = value ? UINT8_C(1) : UINT8_C(0);\n"
                "            *first = left; *second = right;\n"
                "        }\n"
                "        return status;\n",
                index);
        default:
            return false;
    }
}

static bool emit_native_provider_dispatch(CBuffer *buffer, const XrBackendIR *ir,
                                          XrAotNativeProviderKind kind) {
    const char *parameters;
    const char *check = "!result";
    switch (kind) {
        case XR_AOT_NATIVE_I64_NULLARY:
            parameters = "int64_t *result";
            break;
        case XR_AOT_NATIVE_I64_UNARY:
            parameters = "int64_t argument, int64_t *result";
            break;
        case XR_AOT_NATIVE_BOOL_I64_UNARY:
            parameters = "int64_t argument, uint8_t *result";
            break;
        case XR_AOT_NATIVE_OPTIONAL_I64_PAIR_NULLARY:
            parameters = "uint8_t *present, int64_t *first, int64_t *second";
            check = "!present || !first || !second";
            break;
        default:
            return false;
    }
    if (!append_format(buffer,
                       "static int xr_aot_host_%s(void *context, uint32_t requirement, uint32_t "
                       "operation, %s) {\n"
                       "    if (%s) return 1;\n",
                       native_provider_kind_name(kind), parameters, check))
        return false;
    for (size_t index = 0u; index < XR_AOT_NATIVE_PROVIDER_SOURCE_COUNT; ++index) {
        uint32_t requirement, operation;
        if (xr_aot_native_provider_sources[index].kind != kind ||
            !native_provider_requirement(ir, index, &requirement, &operation))
            continue;
        if (!append_format(buffer,
                           "    if (requirement == UINT32_C(%u) && operation == UINT32_C(%u)) {\n",
                           requirement, operation) ||
            !emit_native_provider_call(buffer, kind, index) || !append_text(buffer, "    }\n"))
            return false;
    }
    return append_text(buffer, "    return 1;\n}\n\n");
}

static bool emit_native_provider_definitions(CBuffer *buffer, const XrBackendIR *ir) {
    for (size_t index = 0u; index < XR_AOT_NATIVE_PROVIDER_SOURCE_COUNT; ++index) {
        uint32_t requirement, operation;
        if (native_provider_requirement(ir, index, &requirement, &operation) &&
            !append_text(buffer, xr_aot_native_provider_sources[index].definition))
            return false;
    }
    for (XrAotNativeProviderKind kind = XR_AOT_NATIVE_I64_NULLARY; kind < XR_AOT_NATIVE_KIND_COUNT;
         ++kind)
        if (native_provider_kind_used(ir, kind) && !emit_native_provider_dispatch(buffer, ir, kind))
            return false;
    return true;
}

static bool emit_native_provider_main_bindings(CBuffer *buffer, const XrBackendIR *ir) {
    for (XrAotNativeProviderKind kind = XR_AOT_NATIVE_I64_NULLARY; kind < XR_AOT_NATIVE_KIND_COUNT;
         ++kind) {
        const char *name = native_provider_kind_name(kind);
        if (native_provider_kind_used(ir, kind) &&
            !append_format(buffer, "    xr_ctx.provider_call_%s = xr_aot_host_%s;\n", name, name))
            return false;
    }
    return true;
}
