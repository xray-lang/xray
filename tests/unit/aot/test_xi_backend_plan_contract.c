#include "../../../src/aot/xi_backend_plan_contract.h"

#include <stdio.h>

static int passed;
static int failed;

#define ASSERT_TRUE(cond)                                                                          \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failed++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

#define ASSERT_EQ(actual, expected)                                                                \
    do {                                                                                           \
        if ((actual) != (expected)) {                                                              \
            printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #actual, #expected);              \
            failed++;                                                                              \
            return;                                                                                \
        }                                                                                          \
    } while (0)

static void test_runtime_helper_contract(void) {
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    XaotMethodDispatchPlan plan = {0};

    ASSERT_TRUE(xaot_backend_contract_runtime_helper_allowed(NULL, 11, 2, 7, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_OK);

    plan.kind = XAOT_DISPATCH_RUNTIME_FALLBACK;
    plan.method_name_id = 11;
    plan.arg_count = 2;
    plan.source_span_id = 7;
    ASSERT_TRUE(xaot_backend_contract_runtime_helper_allowed(&plan, 11, 2, 7, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_OK);

    ASSERT_TRUE(!xaot_backend_contract_runtime_helper_allowed(&plan, 12, 2, 7, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_RUNTIME_HELPER_IDENTITY_MISMATCH);

    plan.kind = XAOT_DISPATCH_DIRECT;
    ASSERT_TRUE(!xaot_backend_contract_runtime_helper_allowed(&plan, 11, 2, 7, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_RUNTIME_HELPER_FOR_OPTIMIZED_PLAN);

    passed++;
}

static void test_target_case_contract(void) {
    XaotDispatchTargetCase cases[2] = {{0}};
    XaotBundle bundle = {0};
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    XaotMethodDispatchPlan plan = {0};

    bundle.dispatch_target_cases = cases;
    bundle.ndispatch_target_cases = 2;

    plan.kind = XAOT_DISPATCH_DIRECT;
    plan.target_start = 1;
    plan.target_count = 1;
    ASSERT_TRUE(xaot_backend_contract_check_mandatory_dispatch(
        &bundle, &plan, XAOT_BACKEND_DISPATCH_SUPPORT_DIRECT, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_OK);

    ASSERT_TRUE(!xaot_backend_contract_check_mandatory_dispatch(&bundle, &plan, 0, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_UNSUPPORTED_DISPATCH_KIND);

    plan.target_count = 0;
    ASSERT_TRUE(!xaot_backend_contract_check_mandatory_dispatch(
        &bundle, &plan, XAOT_BACKEND_DISPATCH_SUPPORT_DIRECT, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_MISSING_TARGET_CASES);

    plan.kind = XAOT_DISPATCH_VTABLE;
    plan.target_start = 1;
    plan.target_count = 2;
    ASSERT_TRUE(xaot_backend_contract_check_mandatory_dispatch(
        &bundle, &plan, XAOT_BACKEND_DISPATCH_SUPPORT_VTABLE, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_OK);

    plan.kind = XAOT_DISPATCH_TYPE_SWITCH;
    plan.target_start = 2;
    plan.target_count = 2;
    ASSERT_TRUE(!xaot_backend_contract_check_mandatory_dispatch(
        &bundle, &plan, XAOT_BACKEND_DISPATCH_SUPPORT_TYPE_SWITCH, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_MISSING_TARGET_CASES);

    plan.kind = XAOT_DISPATCH_RUNTIME_FALLBACK;
    plan.target_start = 1;
    plan.target_count = 1;
    ASSERT_TRUE(!xaot_backend_contract_check_mandatory_dispatch(
        &bundle, &plan, XAOT_BACKEND_DISPATCH_SUPPORT_RUNTIME_HELPER, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_UNEXPECTED_TARGET_CASES);

    passed++;
}

static void test_itable_requires_interface_abi_plan(void) {
    XaotInterfaceAbiPlan abi = {0};
    XaotBundle bundle = {0};
    XaotDispatchTargetCase target = {0};
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    XaotMethodDispatchPlan plan = {0};

    plan.kind = XAOT_DISPATCH_ITABLE;
    plan.receiver_static_interface_id = 5;
    plan.target_start = 1;
    plan.target_count = 1;
    bundle.dispatch_target_cases = &target;
    bundle.ndispatch_target_cases = 1;

    ASSERT_TRUE(!xaot_backend_contract_check_mandatory_dispatch(
        &bundle, &plan, XAOT_BACKEND_DISPATCH_SUPPORT_ITABLE, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_MISSING_INTERFACE_ABI_PLAN);

    abi.interface_id = 5;
    abi.itable_source = XAOT_INTERFACE_ABI_SOURCE_NONE;
    abi.evidence = XAOT_INTERFACE_ABI_EV_DISPATCH_PLAN;
    bundle.interface_abi_plans = &abi;
    bundle.ninterface_abi_plans = 1;
    ASSERT_TRUE(!xaot_backend_contract_check_mandatory_dispatch(
        &bundle, &plan, XAOT_BACKEND_DISPATCH_SUPPORT_ITABLE, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_MISSING_INTERFACE_ABI_PLAN);

    abi.itable_source = XAOT_INTERFACE_ABI_SOURCE_DISPATCH_SLOT;
    ASSERT_TRUE(xaot_backend_contract_check_mandatory_dispatch(
        &bundle, &plan, XAOT_BACKEND_DISPATCH_SUPPORT_ITABLE, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_OK);

    plan.target_start = 0;
    plan.target_count = 0;
    ASSERT_TRUE(!xaot_backend_contract_check_mandatory_dispatch(
        &bundle, &plan, XAOT_BACKEND_DISPATCH_SUPPORT_ITABLE, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_MISSING_TARGET_CASES);

    passed++;
}

static void test_profile_contracts(void) {
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    XaotCapabilityPlan cap = {.capability = XG_CAP_CHANNEL,
                              .profile_action = XAOT_CAPABILITY_ACTION_LINK};
    XaotMetadataReachabilityPlan metadata = {.metadata = XG_METADATA_TYPENAME,
                                             .profile_action = XAOT_CAPABILITY_ACTION_ALLOW};
    XaotStaticDataPlan static_data = {.static_data = XG_STATIC_DATA_RUNTIME_INIT,
                                      .action = XAOT_STATIC_DATA_ACTION_RUNTIME_INIT};

    ASSERT_TRUE(xaot_backend_contract_capability_plan_allowed(&cap, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_OK);
    cap.profile_action = XAOT_CAPABILITY_ACTION_REJECT;
    ASSERT_TRUE(!xaot_backend_contract_capability_plan_allowed(&cap, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_CAPABILITY_PROFILE_REJECTED);

    ASSERT_TRUE(xaot_backend_contract_metadata_plan_allowed(&metadata, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_OK);
    metadata.profile_action = XAOT_CAPABILITY_ACTION_REJECT;
    ASSERT_TRUE(!xaot_backend_contract_metadata_plan_allowed(&metadata, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_METADATA_PROFILE_REJECTED);

    ASSERT_TRUE(xaot_backend_contract_static_data_plan_allowed(&static_data, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_OK);
    static_data.action = XAOT_STATIC_DATA_ACTION_REJECT;
    ASSERT_TRUE(!xaot_backend_contract_static_data_plan_allowed(&static_data, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_STATIC_DATA_PROFILE_REJECTED);

    ASSERT_TRUE(!xaot_backend_contract_capability_plan_allowed(NULL, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN);

    passed++;
}

static void test_generic_body_contract(void) {
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    XaotGenericBodyPlan plan = {0};

    plan.root_callsite_id = 11;
    plan.owner_func_id = 7;
    plan.origin_body_func_id = 3;
    plan.specialized_body_func_id = 9;
    plan.action = XAOT_GENERIC_BODY_CLONE;
    ASSERT_TRUE(xaot_backend_contract_generic_body_call_allowed(&plan, 11, 7, 9, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_OK);

    ASSERT_TRUE(!xaot_backend_contract_generic_body_call_allowed(&plan, 11, 7, 3, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_GENERIC_BODY_IDENTITY_MISMATCH);

    plan.action = XAOT_GENERIC_BODY_SHARE_CANONICAL_BODY;
    ASSERT_TRUE(xaot_backend_contract_generic_body_call_allowed(&plan, 11, 7, 3, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_OK);

    ASSERT_TRUE(!xaot_backend_contract_generic_body_call_allowed(&plan, 12, 7, 3, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_GENERIC_BODY_IDENTITY_MISMATCH);

    plan.action = XAOT_GENERIC_BODY_REJECT;
    ASSERT_TRUE(!xaot_backend_contract_generic_body_call_allowed(&plan, 11, 7, 3, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_GENERIC_BODY_ACTION_REJECTED);

    passed++;
}

static void test_json_codec_plan_contracts(void) {
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    uint32_t parse_actions =
        xaot_backend_json_codec_action_bit(XAOT_JSON_CODEC_PARSE_RUNTIME_DIRECT);
    XaotJsonCodecPlan plan = {.codec_id = 1,
                              .owner_func_id = 7,
                              .source_node_id = 99,
                              .codec_kind = XG_JSON_CODEC_PARSE,
                              .action = XAOT_JSON_CODEC_PARSE_RUNTIME_DIRECT,
                              .evidence = XAOT_JSON_EV_GLOBAL_ROW};

    ASSERT_TRUE(!xaot_backend_contract_json_codec_plan_allowed(NULL, XG_JSON_CODEC_PARSE,
                                                               parse_actions, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_MISSING_MANDATORY_PLAN);

    ASSERT_TRUE(xaot_backend_contract_json_codec_plan_allowed(&plan, XG_JSON_CODEC_PARSE,
                                                              parse_actions, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_OK);

    plan.source_node_id = 0;
    ASSERT_TRUE(!xaot_backend_contract_json_codec_plan_allowed(&plan, XG_JSON_CODEC_PARSE,
                                                               parse_actions, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_MANDATORY_PLAN_IDENTITY_MISMATCH);
    plan.source_node_id = 99;

    plan.owner_func_id = XG_NO_ID;
    ASSERT_TRUE(!xaot_backend_contract_json_codec_plan_allowed(&plan, XG_JSON_CODEC_PARSE,
                                                               parse_actions, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_MANDATORY_PLAN_IDENTITY_MISMATCH);
    plan.owner_func_id = 7;

    plan.codec_kind = XG_JSON_CODEC_DECODE;
    ASSERT_TRUE(!xaot_backend_contract_json_codec_plan_allowed(&plan, XG_JSON_CODEC_PARSE,
                                                               parse_actions, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_JSON_CODEC_KIND_MISMATCH);
    plan.codec_kind = XG_JSON_CODEC_PARSE;

    plan.action = XAOT_JSON_CODEC_PARSE_DOM_BRIDGE;
    ASSERT_TRUE(!xaot_backend_contract_json_codec_plan_allowed(&plan, XG_JSON_CODEC_PARSE,
                                                               parse_actions, &issue));
    ASSERT_EQ(issue, XAOT_BACKEND_CONTRACT_JSON_CODEC_ACTION_REJECTED);

    passed++;
}

int main(void) {
    test_runtime_helper_contract();
    test_target_case_contract();
    test_itable_requires_interface_abi_plan();
    test_profile_contracts();
    test_generic_body_contract();
    test_json_codec_plan_contracts();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
