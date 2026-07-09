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
    XaotBackendContractIssue issue = XAOT_BACKEND_CONTRACT_OK;
    XaotMethodDispatchPlan plan = {0};

    plan.kind = XAOT_DISPATCH_ITABLE;
    plan.receiver_static_interface_id = 5;

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

    passed++;
}

int main(void) {
    test_runtime_helper_contract();
    test_target_case_contract();
    test_itable_requires_interface_abi_plan();
    printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
