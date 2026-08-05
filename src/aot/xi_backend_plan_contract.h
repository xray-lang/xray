/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_backend_plan_contract.h - shared verified-plan contract for AOT backends
 *
 * Backends are peer emitters: they may choose how to emit a verified plan, but
 * they must not re-derive class/interface/capability semantics from names or
 * local scans.  This header keeps the mandatory dispatch/runtime-helper gates
 * shared between CGen and future emitters.
 */
#ifndef XI_BACKEND_PLAN_CONTRACT_H
#define XI_BACKEND_PLAN_CONTRACT_H

#include "xaot_bundle.h"

typedef enum XaotBackendDispatchSupport {
    XAOT_BACKEND_DISPATCH_SUPPORT_DIRECT = 1u << 0,
    XAOT_BACKEND_DISPATCH_SUPPORT_VTABLE = 1u << 1,
    XAOT_BACKEND_DISPATCH_SUPPORT_ITABLE = 1u << 2,
    XAOT_BACKEND_DISPATCH_SUPPORT_TYPE_SWITCH = 1u << 3,
    XAOT_BACKEND_DISPATCH_SUPPORT_RUNTIME_HELPER = 1u << 4,
} XaotBackendDispatchSupport;

typedef enum XaotBackendContractIssue {
    XAOT_BACKEND_CONTRACT_OK = 0,
    XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN,
    XAOT_BACKEND_CONTRACT_UNSUPPORTED_DISPATCH_KIND,
    XAOT_BACKEND_CONTRACT_MISSING_TARGET_CASES,
    XAOT_BACKEND_CONTRACT_UNEXPECTED_TARGET_CASES,
    XAOT_BACKEND_CONTRACT_MISSING_INTERFACE_ABI_PLAN,
    XAOT_BACKEND_CONTRACT_RUNTIME_HELPER_FOR_OPTIMIZED_PLAN,
    XAOT_BACKEND_CONTRACT_RUNTIME_HELPER_IDENTITY_MISMATCH,
    XAOT_BACKEND_CONTRACT_CAPABILITY_PROFILE_REJECTED,
    XAOT_BACKEND_CONTRACT_METADATA_PROFILE_REJECTED,
    XAOT_BACKEND_CONTRACT_STATIC_DATA_PROFILE_REJECTED,
    XAOT_BACKEND_CONTRACT_GENERIC_BODY_IDENTITY_MISMATCH,
    XAOT_BACKEND_CONTRACT_GENERIC_BODY_ACTION_REJECTED,
    XAOT_BACKEND_CONTRACT_MANDATORY_PLAN_IDENTITY_MISMATCH,
    XAOT_BACKEND_CONTRACT_JSON_CODEC_KIND_MISMATCH,
    XAOT_BACKEND_CONTRACT_JSON_CODEC_ACTION_REJECTED,
    XAOT_BACKEND_CONTRACT_RECORD_MERGE_ACTION_REJECTED,
} XaotBackendContractIssue;

static inline const char *xaot_backend_contract_issue_name(XaotBackendContractIssue issue) {
    switch (issue) {
        case XAOT_BACKEND_CONTRACT_OK:
            return "ok";
        case XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN:
            return "missing_mandatory_plan";
        case XAOT_BACKEND_CONTRACT_UNSUPPORTED_DISPATCH_KIND:
            return "unsupported_dispatch_kind";
        case XAOT_BACKEND_CONTRACT_MISSING_TARGET_CASES:
            return "missing_target_cases";
        case XAOT_BACKEND_CONTRACT_UNEXPECTED_TARGET_CASES:
            return "unexpected_target_cases";
        case XAOT_BACKEND_CONTRACT_MISSING_INTERFACE_ABI_PLAN:
            return "missing_interface_abi_plan";
        case XAOT_BACKEND_CONTRACT_RUNTIME_HELPER_FOR_OPTIMIZED_PLAN:
            return "runtime_helper_for_optimized_plan";
        case XAOT_BACKEND_CONTRACT_RUNTIME_HELPER_IDENTITY_MISMATCH:
            return "runtime_helper_identity_mismatch";
        case XAOT_BACKEND_CONTRACT_CAPABILITY_PROFILE_REJECTED:
            return "capability_profile_rejected";
        case XAOT_BACKEND_CONTRACT_METADATA_PROFILE_REJECTED:
            return "metadata_profile_rejected";
        case XAOT_BACKEND_CONTRACT_STATIC_DATA_PROFILE_REJECTED:
            return "static_data_profile_rejected";
        case XAOT_BACKEND_CONTRACT_GENERIC_BODY_IDENTITY_MISMATCH:
            return "generic_body_identity_mismatch";
        case XAOT_BACKEND_CONTRACT_GENERIC_BODY_ACTION_REJECTED:
            return "generic_body_action_rejected";
        case XAOT_BACKEND_CONTRACT_MANDATORY_PLAN_IDENTITY_MISMATCH:
            return "mandatory_plan_identity_mismatch";
        case XAOT_BACKEND_CONTRACT_JSON_CODEC_KIND_MISMATCH:
            return "json_codec_kind_mismatch";
        case XAOT_BACKEND_CONTRACT_JSON_CODEC_ACTION_REJECTED:
            return "json_codec_action_rejected";
        case XAOT_BACKEND_CONTRACT_RECORD_MERGE_ACTION_REJECTED:
            return "object_merge_action_rejected";
    }
    return "unknown";
}

static inline void xaot_backend_contract_set_issue(XaotBackendContractIssue *out,
                                                   XaotBackendContractIssue issue) {
    if (out)
        *out = issue;
}

static inline uint32_t xaot_backend_dispatch_support_for_kind(uint8_t kind) {
    switch ((XaotMethodDispatchKind) kind) {
        case XAOT_DISPATCH_DIRECT:
            return XAOT_BACKEND_DISPATCH_SUPPORT_DIRECT;
        case XAOT_DISPATCH_VTABLE:
            return XAOT_BACKEND_DISPATCH_SUPPORT_VTABLE;
        case XAOT_DISPATCH_ITABLE:
            return XAOT_BACKEND_DISPATCH_SUPPORT_ITABLE;
        case XAOT_DISPATCH_TYPE_SWITCH:
            return XAOT_BACKEND_DISPATCH_SUPPORT_TYPE_SWITCH;
        case XAOT_DISPATCH_RUNTIME_FALLBACK:
            return XAOT_BACKEND_DISPATCH_SUPPORT_RUNTIME_HELPER;
    }
    return 0;
}

static inline bool xaot_backend_dispatch_kind_requires_emitter(uint8_t kind) {
    return kind == XAOT_DISPATCH_DIRECT || kind == XAOT_DISPATCH_VTABLE ||
           kind == XAOT_DISPATCH_ITABLE || kind == XAOT_DISPATCH_TYPE_SWITCH;
}

static inline bool
xaot_backend_dispatch_plan_target_range_valid(const XaotBundle *bundle,
                                              const XaotMethodDispatchPlan *plan,
                                              XaotBackendContractIssue *out_issue) {
    if (!plan) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN);
        return false;
    }

    switch ((XaotMethodDispatchKind) plan->kind) {
        case XAOT_DISPATCH_DIRECT:
            if (bundle && plan->target_count == 1 && plan->target_start > 0 &&
                plan->target_start - 1 < bundle->ndispatch_target_cases) {
                xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
                return true;
            }
            xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_MISSING_TARGET_CASES);
            return false;

        case XAOT_DISPATCH_VTABLE:
        case XAOT_DISPATCH_ITABLE:
        case XAOT_DISPATCH_TYPE_SWITCH:
            if (bundle && plan->target_count > 0 && plan->target_start > 0 &&
                plan->target_start - 1 + plan->target_count <= bundle->ndispatch_target_cases) {
                xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
                return true;
            }
            xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_MISSING_TARGET_CASES);
            return false;

        case XAOT_DISPATCH_RUNTIME_FALLBACK:
            if (plan->target_start == 0 && plan->target_count == 0) {
                xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
                return true;
            }
            xaot_backend_contract_set_issue(out_issue,
                                            XAOT_BACKEND_CONTRACT_UNEXPECTED_TARGET_CASES);
            return false;
    }

    xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_UNSUPPORTED_DISPATCH_KIND);
    return false;
}

static inline bool
xaot_backend_dispatch_plan_itable_abi_valid(const XaotBundle *bundle,
                                            const XaotMethodDispatchPlan *plan,
                                            XaotBackendContractIssue *out_issue) {
    if (!plan || plan->kind != XAOT_DISPATCH_ITABLE) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
        return true;
    }
    if (!bundle || plan->receiver_static_interface_id == XG_NO_ID) {
        xaot_backend_contract_set_issue(out_issue,
                                        XAOT_BACKEND_CONTRACT_MISSING_INTERFACE_ABI_PLAN);
        return false;
    }
    const XaotInterfaceAbiPlan *abi =
        xaot_bundle_find_interface_abi_plan(bundle, plan->receiver_static_interface_id);
    if (!abi || abi->itable_source == XAOT_INTERFACE_ABI_SOURCE_NONE ||
        !(abi->evidence & XAOT_INTERFACE_ABI_EV_DISPATCH_PLAN)) {
        xaot_backend_contract_set_issue(out_issue,
                                        XAOT_BACKEND_CONTRACT_MISSING_INTERFACE_ABI_PLAN);
        return false;
    }
    xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
    return true;
}

static inline bool xaot_backend_contract_check_mandatory_dispatch(
    const XaotBundle *bundle, const XaotMethodDispatchPlan *plan, uint32_t supported_dispatch,
    XaotBackendContractIssue *out_issue) {
    if (!plan) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN);
        return false;
    }

    uint32_t required = xaot_backend_dispatch_support_for_kind(plan->kind);
    if (required == 0 || !(supported_dispatch & required)) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_UNSUPPORTED_DISPATCH_KIND);
        return false;
    }

    if (!xaot_backend_dispatch_plan_target_range_valid(bundle, plan, out_issue))
        return false;

    if (!xaot_backend_dispatch_plan_itable_abi_valid(bundle, plan, out_issue))
        return false;

    xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
    return true;
}

static inline bool xaot_backend_contract_runtime_helper_allowed(
    const XaotMethodDispatchPlan *plan, uint32_t method_name_id, uint16_t arg_count,
    uint32_t source_span_id, XaotBackendContractIssue *out_issue) {
    if (!plan) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
        return true;
    }
    if (plan->kind != XAOT_DISPATCH_RUNTIME_FALLBACK) {
        xaot_backend_contract_set_issue(out_issue,
                                        XAOT_BACKEND_CONTRACT_RUNTIME_HELPER_FOR_OPTIMIZED_PLAN);
        return false;
    }
    if (plan->method_name_id == 0 || plan->method_name_id != method_name_id ||
        plan->arg_count != arg_count ||
        (source_span_id != 0 && plan->source_span_id != 0 &&
         plan->source_span_id != source_span_id)) {
        xaot_backend_contract_set_issue(out_issue,
                                        XAOT_BACKEND_CONTRACT_RUNTIME_HELPER_IDENTITY_MISMATCH);
        return false;
    }
    xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
    return true;
}

static inline bool
xaot_backend_contract_capability_plan_allowed(const XaotCapabilityPlan *plan,
                                              XaotBackendContractIssue *out_issue) {
    if (!plan) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN);
        return false;
    }
    if (plan->profile_action == XAOT_CAPABILITY_ACTION_REJECT) {
        xaot_backend_contract_set_issue(out_issue,
                                        XAOT_BACKEND_CONTRACT_CAPABILITY_PROFILE_REJECTED);
        return false;
    }
    xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
    return true;
}

static inline bool
xaot_backend_contract_metadata_plan_allowed(const XaotMetadataReachabilityPlan *plan,
                                            XaotBackendContractIssue *out_issue) {
    if (!plan) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN);
        return false;
    }
    if (plan->profile_action == XAOT_CAPABILITY_ACTION_REJECT) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_METADATA_PROFILE_REJECTED);
        return false;
    }
    xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
    return true;
}

static inline bool
xaot_backend_contract_static_data_plan_allowed(const XaotStaticDataPlan *plan,
                                               XaotBackendContractIssue *out_issue) {
    if (!plan) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN);
        return false;
    }
    if (plan->action == XAOT_STATIC_DATA_ACTION_REJECT) {
        xaot_backend_contract_set_issue(out_issue,
                                        XAOT_BACKEND_CONTRACT_STATIC_DATA_PROFILE_REJECTED);
        return false;
    }
    xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
    return true;
}

static inline bool xaot_backend_contract_generic_body_call_allowed(
    const XaotGenericBodyPlan *plan, XgCallsiteId callsite_id, XgFuncId owner_func_id,
    XgFuncId target_func_id, XaotBackendContractIssue *out_issue) {
    /* Inlining can move a value into a different XiFunc by CGen time. The
     * verified root callsite id remains the stable generic-body anchor. */
    (void) owner_func_id;
    if (!plan) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN);
        return false;
    }
    if (callsite_id == XG_NO_ID || target_func_id == XG_NO_ID ||
        plan->root_callsite_id != callsite_id) {
        xaot_backend_contract_set_issue(out_issue,
                                        XAOT_BACKEND_CONTRACT_GENERIC_BODY_IDENTITY_MISMATCH);
        return false;
    }

    switch ((XaotGenericBodyAction) plan->action) {
        case XAOT_GENERIC_BODY_CLONE:
            if (plan->specialized_body_func_id != XG_NO_ID &&
                target_func_id == plan->specialized_body_func_id) {
                xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
                return true;
            }
            xaot_backend_contract_set_issue(out_issue,
                                            XAOT_BACKEND_CONTRACT_GENERIC_BODY_IDENTITY_MISMATCH);
            return false;
        case XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY:
        case XAOT_GENERIC_BODY_DIRECT_CONSTRAINT_CALL:
            if (plan->origin_body_func_id != XG_NO_ID &&
                target_func_id == plan->origin_body_func_id) {
                xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
                return true;
            }
            xaot_backend_contract_set_issue(out_issue,
                                            XAOT_BACKEND_CONTRACT_GENERIC_BODY_IDENTITY_MISMATCH);
            return false;
        case XAOT_GENERIC_BODY_REJECT:
            xaot_backend_contract_set_issue(out_issue,
                                            XAOT_BACKEND_CONTRACT_GENERIC_BODY_ACTION_REJECTED);
            return false;
    }

    xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_GENERIC_BODY_ACTION_REJECTED);
    return false;
}

static inline uint32_t xaot_backend_json_codec_action_bit(uint8_t action) {
    return action < 32 ? UINT32_C(1) << action : 0;
}

static inline bool
xaot_backend_contract_json_codec_plan_allowed(const XaotJsonCodecPlan *plan, uint8_t expected_kind,
                                              uint32_t allowed_actions,
                                              XaotBackendContractIssue *out_issue) {
    if (!plan) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN);
        return false;
    }
    if (plan->owner_func_id == XG_NO_ID || plan->source_node_id == 0 ||
        (plan->evidence & XAOT_JSON_EV_GLOBAL_ROW) == 0) {
        xaot_backend_contract_set_issue(out_issue,
                                        XAOT_BACKEND_CONTRACT_MANDATORY_PLAN_IDENTITY_MISMATCH);
        return false;
    }
    if (plan->codec_kind != expected_kind) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_JSON_CODEC_KIND_MISMATCH);
        return false;
    }
    if ((allowed_actions & xaot_backend_json_codec_action_bit(plan->action)) == 0) {
        xaot_backend_contract_set_issue(out_issue,
                                        XAOT_BACKEND_CONTRACT_JSON_CODEC_ACTION_REJECTED);
        return false;
    }
    xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
    return true;
}

static inline uint32_t xaot_backend_object_merge_action_bit(uint8_t action) {
    return action < 32 ? UINT32_C(1) << action : 0;
}

static inline bool
xaot_backend_contract_object_merge_plan_allowed(const XaotObjectMergePlan *plan,
                                                uint32_t allowed_actions,
                                                XaotBackendContractIssue *out_issue) {
    const uint32_t required_evidence = XAOT_OBJECT_EV_GLOBAL_ROW | XAOT_OBJECT_EV_BASE_SHAPE |
                                       XAOT_OBJECT_EV_PATCH_SHAPE | XAOT_OBJECT_EV_RESULT_SHAPE |
                                       XAOT_OBJECT_EV_COPY_TABLE;
    if (!plan) {
        xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN);
        return false;
    }
    if (plan->owner_func_id == XG_NO_ID || plan->source_node_id == 0 ||
        (plan->evidence & required_evidence) != required_evidence || plan->copy_table_id == 0) {
        xaot_backend_contract_set_issue(out_issue,
                                        XAOT_BACKEND_CONTRACT_MANDATORY_PLAN_IDENTITY_MISMATCH);
        return false;
    }
    if ((allowed_actions & xaot_backend_object_merge_action_bit(plan->action)) == 0 ||
        plan->unproven_reason != XAOT_OBJECT_UNPROVEN_NONE) {
        xaot_backend_contract_set_issue(out_issue,
                                        XAOT_BACKEND_CONTRACT_RECORD_MERGE_ACTION_REJECTED);
        return false;
    }
    xaot_backend_contract_set_issue(out_issue, XAOT_BACKEND_CONTRACT_OK);
    return true;
}

#endif /* XI_BACKEND_PLAN_CONTRACT_H */
