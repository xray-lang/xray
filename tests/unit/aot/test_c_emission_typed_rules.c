#include "../../../src/aot/emit_c/xr_c_emission_rule_runtime.h"

#include <stdio.h>
#include <stdlib.h>

static void require(bool condition) {
    if (!condition) {
        fputs("test_c_emission_typed_rules: assertion failed\n", stderr);
        abort();
    }
}

static XrCEmissionRuleFacts exact_push_facts(void) {
    return (XrCEmissionRuleFacts) {
        .member = XI_METHOD_SYMBOL_PUSH,
        .opcode = XI_CALL_METHOD,
        .operand_count = 2,
        .result_kind = XR_KIND_UNIT,
        .intrinsic = XR_SEM_INTRINSIC_ARRAY_MEMBER_SCALAR,
        .element_access = XR_ARRAY_MEMBER_ELEMENT_ACCESS_STORE,
        .reference_action = XR_ARRAY_MEMBER_REFERENCE_CONSUME_INTO_STORAGE,
        .reference_drop = XR_ARRAY_MEMBER_REFERENCE_DROP_RELEASE_ON_ERASE_OR_DESTROY,
        .element_managed_reference = true,
        .call_convention = XR_TARGET_CALL_CONVENTION_ARRAY_MEMBER_SCALAR,
        .target_kind = XR_TARGET_CALL_TARGET_ARRAY_MEMBER_SCALAR,
        .layout_kind = XR_TARGET_LAYOUT_DYNAMIC,
        .call_storage = XR_TARGET_ARRAY_STORAGE_TAGGED,
        .layout_storage = XR_TARGET_ARRAY_STORAGE_TAGGED,
        .argument_ownership = {XR_TARGET_CALL_BORROW, XR_TARGET_CALL_CONSUME},
        .argument_storage = {XR_TARGET_ARRAY_STORAGE_NONE, XR_TARGET_ARRAY_STORAGE_TAGGED},
        .caller_register_kind = {XR_MACHINE_REP_DYN_VALUE, XR_MACHINE_REP_DYN_VALUE},
        .caller_memory_kind = {XR_MACHINE_REP_DYN_VALUE, XR_MACHINE_REP_DYN_VALUE},
        .operation_result_bound = true,
        .call_result_bound = true,
        .arguments_structurally_exact = true,
    };
}

static void require_fact_mutation_rejected(XrCEmissionRuleFacts facts) {
    XrCEmissionRuleDecision decision = {0};
    require(xr_c_emission_rule_build(&facts, &decision) == XR_C_EMISSION_RULE_MALFORMED);
    XrCEmissionRuleDecision exact = {
        XR_C_EMISSION_RULE_C_EMISSION_ARRAY_PUSH_TAGGED_V1,
        XR_C_VALUE_MATERIALIZATION_ARRAY_PUSH_TAGGED,
        XR_C_VALUE_REP_VOID,
        XR_TARGET_ARRAY_STORAGE_TAGGED,
        "xrt_array_push",
    };
    require(xr_c_emission_rule_verify(&facts, &exact, NULL) == XR_C_EMISSION_RULE_MALFORMED);
}

static void test_exact_and_decision_mutations(void) {
    XrCEmissionRuleFacts facts = exact_push_facts();
    XrCEmissionRuleDecision decision = {0};
    require(xr_c_emission_rule_build(&facts, &decision) == XR_C_EMISSION_RULE_EXACT);
    require(xr_c_emission_rule_verify(&facts, &decision, NULL) == XR_C_EMISSION_RULE_EXACT);

    XrCEmissionRuleDecision mutation = decision;
    mutation.rule_id = XR_C_EMISSION_RULE_NONE;
    require(xr_c_emission_rule_verify(&facts, &mutation, NULL) == XR_C_EMISSION_RULE_MALFORMED);
    mutation = decision;
    mutation.recipe = XR_C_VALUE_MATERIALIZATION_NONE;
    require(xr_c_emission_rule_verify(&facts, &mutation, NULL) == XR_C_EMISSION_RULE_MALFORMED);
    mutation = decision;
    mutation.rep = XR_C_VALUE_REP_TAGGED;
    require(xr_c_emission_rule_verify(&facts, &mutation, NULL) == XR_C_EMISSION_RULE_MALFORMED);
    mutation = decision;
    mutation.storage = XR_TARGET_ARRAY_STORAGE_NONE;
    require(xr_c_emission_rule_verify(&facts, &mutation, NULL) == XR_C_EMISSION_RULE_MALFORMED);
    mutation = decision;
    mutation.symbol = "xrt_array_set";
    require(xr_c_emission_rule_verify(&facts, &mutation, NULL) == XR_C_EMISSION_RULE_MALFORMED);
}

static void test_clause_mutations(void) {
    XrCEmissionRuleFacts facts = exact_push_facts();
    facts.opcode = XI_CALL;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.intrinsic = XR_SEM_INTRINSIC_NONE;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.operand_count = 1;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.result_kind = XR_KIND_INT;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.element_access = XR_ARRAY_MEMBER_ELEMENT_ACCESS_READ;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.reference_action = XR_ARRAY_MEMBER_REFERENCE_PRESERVE;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.reference_drop = XR_ARRAY_MEMBER_REFERENCE_DROP_NONE;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.element_managed_reference = false;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.call_convention = XR_TARGET_CALL_CONVENTION_DIRECT_LOCAL;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.target_kind = XR_TARGET_CALL_TARGET_DIRECT_LOCAL;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.layout_kind = XR_TARGET_LAYOUT_SCALAR;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.call_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.layout_storage = XR_TARGET_ARRAY_STORAGE_NONE;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.argument_ownership[1] = XR_TARGET_CALL_BORROW;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.argument_storage[1] = XR_TARGET_ARRAY_STORAGE_NONE;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.caller_register_kind[1] = XR_MACHINE_REP_RAW_PTR;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.caller_memory_kind[1] = XR_MACHINE_REP_RAW_PTR;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.operation_result_bound = false;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.call_result_bound = false;
    require_fact_mutation_rejected(facts);
    facts = exact_push_facts();
    facts.arguments_structurally_exact = false;
    require_fact_mutation_rejected(facts);
}

static void test_closed_domain(void) {
    XrCEmissionRuleFacts facts = exact_push_facts();
    facts.member = XI_METHOD_SYMBOL_POP;
    XrCEmissionRuleDecision decision = {0};
    require(xr_c_emission_rule_build(&facts, &decision) ==
            XR_C_EMISSION_RULE_NOT_APPLICABLE);
    require(xr_c_emission_rule_verify(&facts, &decision, NULL) ==
            XR_C_EMISSION_RULE_NOT_APPLICABLE);
}

static void test_scalar_push_is_outside_tagged_domain(void) {
    XrCEmissionRuleFacts facts = exact_push_facts();
    facts.element_managed_reference = false;
    facts.call_storage = XR_TARGET_ARRAY_STORAGE_U8;
    facts.layout_storage = XR_TARGET_ARRAY_STORAGE_U8;
    facts.argument_storage[1] = XR_TARGET_ARRAY_STORAGE_U8;
    facts.caller_register_kind[1] = XR_MACHINE_REP_U8;
    facts.caller_memory_kind[1] = XR_MACHINE_REP_U8;
    XrCEmissionRuleDecision decision = {0};
    require(xr_c_emission_rule_build(&facts, &decision) ==
            XR_C_EMISSION_RULE_NOT_APPLICABLE);
    require(xr_c_emission_rule_verify(&facts, &decision, NULL) ==
            XR_C_EMISSION_RULE_NOT_APPLICABLE);
}

int main(void) {
    test_exact_and_decision_mutations();
    test_clause_mutations();
    test_closed_domain();
    test_scalar_push_is_outside_tagged_domain();
    puts("test_c_emission_typed_rules: 4 passed");
    return 0;
}
