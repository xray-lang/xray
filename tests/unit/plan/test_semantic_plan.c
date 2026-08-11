/*
 * test_semantic_plan.c - Immutable SemanticPlan and exact-version XSM contract
 */

#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xsha256.h"
#include "../../../src/base/xstorage.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_arc.h"
#include "../../../src/ir/xi_effect.h"
#include "../../../src/plan/format/xr_xsm_schema.h"
#include "../../../src/plan/ownership/xr_ownership_certificate.h"
#include "../../../src/plan/ownership/xr_ownership_certificate_internal.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
#include "../../../src/plan/semantic/xr_semantic_graph.h"
#include "../../../src/plan/semantic/xr_semantic_ops.h"
#include "../../../src/plan/semantic/xr_semantic_plan_internal.h"
#include "../../../src/plan/semantic/xr_semantic_verify.h"
#include "../../../src/runtime/value/xtype.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REQUIRE(condition)                                                                         \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            fprintf(stderr, "requirement failed at %s:%d: %s\n", __FILE__, __LINE__, #condition);  \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static XrType stub_int = {.kind = XR_KIND_INT, .id = 1, .frozen = true};
static XrType stub_bool = {.kind = XR_KIND_BOOL, .id = 2, .frozen = true};
static XrType stub_string = {.kind = XR_KIND_STRING, .id = 3, .frozen = true};
static XrType stub_unit = {.kind = XR_KIND_UNIT, .id = 4, .frozen = true};
static XrType stub_null = {.kind = XR_KIND_NULL, .id = 6, .frozen = true};
static XrType stub_function = {
    .kind = XR_KIND_FUNCTION,
    .id = 7,
    .frozen = true,
    .function = {.return_type = &stub_int, .throw_effect = XR_FN_EFFECT_NO_THROW},
};
static XrType stub_array = {
    .kind = XR_KIND_ARRAY,
    .id = 5,
    .frozen = true,
    .container = {.element_type = &stub_int},
};

static XrSemanticPlan *build_probe_plan(void) {
    XiFunc *function = xi_func_new("artifact_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    REQUIRE(xi_const_bool(function, entry, true, &stub_bool) != NULL);
    REQUIRE(xi_const_str(function, entry, "owned-by-plan", &stub_string) != NULL);
    XiValue *result = xi_const_int(function, entry, 42, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "SemanticPlan build failed: %s\n", error);
    REQUIRE(built);
    REQUIRE(plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_owned_parameter_plan(void) {
    XiFunc *function = xi_func_new("owned_parameter_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *parameter = xi_param(function, entry, 0, &stub_string);
    REQUIRE(parameter != NULL);
    function->nparams = 1;
    function->params = (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = parameter;
    XiValue *result = xi_const_int(function, entry, 7, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "owned-parameter plan build failed: %s\n", error);
    REQUIRE(built);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_panic_edge_plan(void) {
    XiFunc *function = xi_func_new("panic_edge_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *registration = xi_block_new(function);
    XiBlock *body = xi_block_new(function);
    XiBlock *handler = xi_block_new(function);
    REQUIRE(registration != NULL && body != NULL && handler != NULL);

    XiValue *try_operation = xi_value_new(function, registration, XI_TRY, &stub_unit, 0);
    REQUIRE(try_operation != NULL);
    try_operation->aux = handler;
    try_operation->aux_int = -1;
    try_operation->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_jump(registration, body);

    XiValue *normal = xi_const_int(function, body, 1, &stub_int);
    XiValue *caught = xi_const_int(function, handler, 2, &stub_int);
    REQUIRE(normal != NULL && caught != NULL);
    xi_block_set_return(body, normal);
    xi_block_set_return(handler, caught);
    /* This is the same SSA-only predecessor shape produced by panic lowering:
     * it is deliberately not the block containing XI_TRY. */
    xi_block_add_pred(handler, body);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "panic-edge plan build failed: %s\n", error);
    REQUIRE(built);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_error_edge_plan(void) {
    XiFunc *function = xi_func_new("error_edge_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *check_block = xi_block_new(function);
    XiBlock *error_block = xi_block_new(function);
    XiBlock *normal_block = xi_block_new(function);
    REQUIRE(check_block != NULL && error_block != NULL && normal_block != NULL);

    XiValue *check = xi_value_new(function, check_block, XI_ERR_CHECK, &stub_bool, 0);
    REQUIRE(check != NULL);
    check->flags = XI_FLAG_SIDE_EFFECT;
    xi_block_set_if(check_block, check, error_block, normal_block);
    XiValue *error_result = xi_const_int(function, error_block, -1, &stub_int);
    XiValue *normal_result = xi_const_int(function, normal_block, 1, &stub_int);
    REQUIRE(error_result != NULL && normal_result != NULL);
    xi_block_set_return(error_block, error_result);
    xi_block_set_return(normal_block, normal_result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "error-edge plan build failed: %s\n", error);
    REQUIRE(built);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_phi_dominance_plan(void) {
    XiFunc *function = xi_func_new("phi_dominance_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *left = xi_block_new(function);
    XiBlock *right = xi_block_new(function);
    XiBlock *merge = xi_block_new(function);
    REQUIRE(entry != NULL && left != NULL && right != NULL && merge != NULL);

    XiValue *condition = xi_const_bool(function, entry, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(entry, condition, left, right);
    XiValue *left_value = xi_const_int(function, left, 10, &stub_int);
    XiValue *right_value = xi_const_int(function, right, 20, &stub_int);
    REQUIRE(left_value != NULL && right_value != NULL);
    xi_block_set_jump(left, merge);
    xi_block_set_jump(right, merge);
    XiPhi *phi = xi_phi_new(function, merge, &stub_int, merge->npreds);
    REQUIRE(phi != NULL && merge->npreds == 2);
    phi->value.args[0] = left_value;
    phi->value.args[1] = right_value;
    xi_block_set_return(merge, &phi->value);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "PHI dominance plan build failed: %s\n", error);
    REQUIRE(built);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_typed_call_operand_plan(void) {
    XiFunc *function = xi_func_new("typed_call_operand_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiImportRef import_ref = {
        .module_path = "./operand_contract",
        .member_name = "target",
        .resolved_mod_index = -1,
        .resolved_shared_slot = -1,
        .resolved_export_slot = -1,
    };
    XiValue *callee = xi_value_new(function, entry, XI_IMPORT_REF, &stub_function, 0);
    XiValue *first = xi_const_int(function, entry, 11, &stub_int);
    XiValue *second = xi_const_int(function, entry, 22, &stub_int);
    REQUIRE(callee != NULL && first != NULL && second != NULL);
    callee->aux = &import_ref;

    XiCallArgPlan arguments[2] = {0};
    arguments[0].param_mode = XR_PARAM_REF;
    arguments[0].access = XR_CALL_ARG_REF;
    arguments[0].origin = XI_PLACE_ORIGIN_STACK_LOCAL;
    arguments[0].lifetime = XI_PLACE_LIFETIME_CALL_BOUND;
    arguments[0].addressable = true;
    arguments[0].origin_var_id = 0;
    arguments[0].place = first;
    arguments[1].param_mode = XR_PARAM_MOVE;
    arguments[1].access = XR_CALL_ARG_MOVE;
    arguments[1].origin_var_id = XI_NO_VAR_ID;
    XiCallPlan call_plan = {.args = arguments, .nargs = 2, .verified = true};

    XiValue *call = xi_value_new(function, entry, XI_CALL, &stub_int, 3);
    REQUIRE(call != NULL);
    call->args[0] = callee;
    call->args[1] = first;
    call->args[2] = second;
    call->call_plan = &call_plan;
    xi_block_set_return(entry, call);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "typed-call-operand plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(function);
    return plan;
}

static XrSemanticPlan *build_capture_contract_plan(void) {
    XiFunc *root = xi_func_new("capture_contract_root", &stub_int);
    XiFunc *child = xi_func_new("capture_contract_child", &stub_int);
    XiFunc *grandchild = xi_func_new("capture_contract_grandchild", &stub_int);
    REQUIRE(root != NULL && child != NULL && grandchild != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    XiBlock *grandchild_entry = xi_block_new(grandchild);
    REQUIRE(root_entry != NULL && child_entry != NULL && grandchild_entry != NULL);
    XiValue *captured = xi_const_str(root, root_entry, "captured", &stub_string);
    XiValue *root_result = xi_const_int(root, root_entry, 1, &stub_int);
    XiValue *child_result = xi_const_int(child, child_entry, 2, &stub_int);
    XiValue *grandchild_result = xi_const_int(grandchild, grandchild_entry, 3, &stub_int);
    REQUIRE(captured != NULL && root_result != NULL && child_result != NULL &&
            grandchild_result != NULL);
    xi_block_set_return(root_entry, root_result);
    xi_block_set_return(child_entry, child_result);
    xi_block_set_return(grandchild_entry, grandchild_result);

    root->children = (XiFunc **) xr_malloc(sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = 1;
    root->children_cap = 1;
    child->parent_func = root;
    child->children = (XiFunc **) xr_malloc(sizeof(*child->children));
    REQUIRE(child->children != NULL);
    child->children[0] = grandchild;
    child->nchildren = 1;
    child->children_cap = 1;
    grandchild->parent_func = child;
    child->ncaptures = 1;
    child->captures[0].source = XI_CAPTURE_SRC_REG;
    child->captures[0].capture_kind = XI_CAPTURE_BY_COPY;
    child->captures[0].name = "captured";
    child->captures[0].type = &stub_string;
    child->captures[0].value = captured;
    child->captures[0].storage_domain = XR_STORAGE_EXEC_LOCAL;
    child->captures[0].value_capability = XR_SEM_VALUE_CONST;
    grandchild->ncaptures = 1;
    grandchild->captures[0].source = XI_CAPTURE_SRC_UPVAL;
    grandchild->captures[0].index = 0;
    grandchild->captures[0].capture_kind = XI_CAPTURE_BY_COPY;
    grandchild->captures[0].name = "captured";
    grandchild->captures[0].type = &stub_string;
    grandchild->captures[0].storage_domain = XR_STORAGE_EXEC_LOCAL;
    grandchild->captures[0].value_capability = XR_SEM_VALUE_CONST;
    root->stage = XI_STAGE_OPTIMIZED;
    child->stage = XI_STAGE_OPTIMIZED;
    grandchild->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(root, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "capture-contract plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    xi_func_free(root);
    return plan;
}

static uint8_t *copy_bytes(const uint8_t *bytes, size_t size) {
    uint8_t *copy = (uint8_t *) xr_malloc(size);
    REQUIRE(copy != NULL);
    memcpy(copy, bytes, size);
    return copy;
}

static void rewrite_payload_digest(uint8_t *bytes, size_t size) {
    REQUIRE(size >= XR_XSM_HEADER_SIZE);
    xr_sha256(bytes + XR_XSM_HEADER_SIZE, size - XR_XSM_HEADER_SIZE, bytes + 24);
}

static size_t find_bytes(const uint8_t *bytes, size_t size, const char *needle) {
    size_t length = strlen(needle);
    if (length == 0 || length > size)
        return SIZE_MAX;
    for (size_t i = 0; i <= size - length; i++) {
        if (memcmp(bytes + i, needle, length) == 0)
            return i;
    }
    return SIZE_MAX;
}

static char *dump_plan(const XrSemanticPlan *plan, size_t *size) {
    FILE *stream = tmpfile();
    REQUIRE(stream != NULL);
    REQUIRE(xr_semantic_plan_dump(plan, stream));
    REQUIRE(fflush(stream) == 0);
    REQUIRE(fseek(stream, 0, SEEK_END) == 0);
    long end = ftell(stream);
    REQUIRE(end >= 0);
    REQUIRE(fseek(stream, 0, SEEK_SET) == 0);
    char *text = (char *) xr_malloc((size_t) end + 1u);
    REQUIRE(text != NULL);
    REQUIRE(fread(text, 1, (size_t) end, stream) == (size_t) end);
    text[end] = '\0';
    fclose(stream);
    if (size)
        *size = (size_t) end;
    return text;
}

static void expect_decode_failure(const uint8_t *bytes, size_t size, const char *code) {
    XrSemanticPlan *decoded = NULL;
    char error[512] = {0};
    REQUIRE(!xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(decoded == NULL);
    REQUIRE(strncmp(error, code, strlen(code)) == 0);
}

static void expect_verify_failure(XrSemanticPlan *plan, const char *code);

static void test_stable_ids(void) {
    XrStableId id;
    XrFingerprint digest;
    char hex[XR_STABLE_ID_BYTES * 2 + 1];
    REQUIRE(xr_stable_id_from_key("alpha", &id, &digest));
    xr_stable_id_hex(id, hex);
    REQUIRE(strcmp(hex, "9bf6af74f9708ff32171293971b35071") == 0);

    XrSemanticPlan *plan = build_probe_plan();
    const XrSemanticFunctionRecord *function = xr_semantic_plan_function(plan, 0);
    REQUIRE(function != NULL);
    xr_stable_id_hex(function->id, hex);
    REQUIRE(strcmp(hex, "cf96f5ef90be2fedbf7f7f632cfd5fb6") == 0);
    xr_semantic_plan_free(plan);
}

static void test_immutable_owned_snapshot(void) {
    XrSemanticPlan *plan = build_probe_plan();
    REQUIRE(xr_semantic_plan_is_frozen(plan));
    REQUIRE(xr_semantic_plan_is_verified(plan));
    REQUIRE(xr_semantic_plan_schema(plan) == XR_SEMANTIC_SCHEMA_VERSION);
    XrFingerprint registry_fingerprint;
    xr_semantic_op_registry_fingerprint(&registry_fingerprint);
    REQUIRE(xr_fingerprint_equal(registry_fingerprint,
                                 xr_semantic_plan_operation_registry_fingerprint(plan)));
    REQUIRE(xr_semantic_plan_function_count(plan) == 1);
    REQUIRE(xr_semantic_plan_block_count(plan) == 1);
    REQUIRE(xr_semantic_plan_operation_count(plan) == 3);
    REQUIRE(xr_semantic_plan_constant_count(plan) == 3);

    const XrSemanticConstantRecord *string_constant = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_constant_count(plan); i++) {
        const XrSemanticConstantRecord *candidate = xr_semantic_plan_constant(plan, i);
        if (candidate && candidate->kind == XR_SEM_CONST_STRING)
            string_constant = candidate;
    }
    REQUIRE(string_constant != NULL);
    REQUIRE(strcmp(string_constant->string, "owned-by-plan") == 0);

    for (uint32_t i = 1; i < xr_semantic_plan_type_count(plan); i++) {
        const XrSemanticTypeRecord *previous = xr_semantic_plan_type(plan, i - 1);
        const XrSemanticTypeRecord *current = xr_semantic_plan_type(plan, i);
        REQUIRE(previous != NULL && current != NULL);
        REQUIRE(xr_stable_id_compare(previous->id, current->id) < 0);
    }
    const XrOwnershipCertificate *ownership = xr_semantic_plan_ownership(plan);
    REQUIRE(ownership != NULL);
    REQUIRE(xr_ownership_certificate_owner_count(ownership) == 1);
    xr_semantic_plan_free(plan);
}

static void test_operation_registry(void) {
    char error[512] = {0};
    REQUIRE(xr_semantic_op_registry_verify(error, sizeof(error)));
    REQUIRE(xr_semantic_op_contract_count() == XI_OP_COUNT);

    const XrSemanticOpContract *add = xr_semantic_op_contract(XI_ADD);
    const XrSemanticOpContract *decode = xr_semantic_op_contract(XI_JSON_DECODE);
    const XrSemanticOpContract *print = xr_semantic_op_contract(XI_PRINT);
    const XrSemanticOpContract *generated = xr_semantic_op_contract(XI_GEN_CALL);
    REQUIRE(add != NULL && strcmp(add->canonical_name, "xi.add") == 0);
    REQUIRE(add->owner == XR_SEM_OWNER_DECLARATIVE_PRIMITIVE);
    REQUIRE(decode != NULL && decode->owner == XR_SEM_OWNER_SHARED_SEMANTIC_KERNEL);
    REQUIRE(print != NULL && print->owner == XR_SEM_OWNER_CAPABILITY_PROVIDER);
    REQUIRE(generated != NULL && generated->owner == XR_SEM_OWNER_GENERATED_SPECIALIZATION);
    REQUIRE(xr_semantic_op_contract(XI_OP_COUNT) == NULL);
}

static void test_xsm_roundtrip_and_determinism(void) {
    XrSemanticPlan *first = build_probe_plan();
    XrSemanticPlan *second = build_probe_plan();
    REQUIRE(xr_fingerprint_equal(xr_semantic_plan_fingerprint(first),
                                 xr_semantic_plan_fingerprint(second)));

    uint8_t *first_bytes = NULL;
    uint8_t *second_bytes = NULL;
    size_t first_size = 0;
    size_t second_size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(first, &first_bytes, &first_size, error, sizeof(error)));
    REQUIRE(xr_xsm_encode(second, &second_bytes, &second_size, error, sizeof(error)));
    REQUIRE(first_size == second_size);
    REQUIRE(memcmp(first_bytes, second_bytes, first_size) == 0);
    REQUIRE(first_size > XR_XSM_HEADER_SIZE);

    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(first_bytes, first_size, &decoded, error, sizeof(error)));
    REQUIRE(decoded != NULL);
    REQUIRE(xr_semantic_plan_is_verified(decoded));
    REQUIRE(xr_fingerprint_equal(xr_semantic_plan_fingerprint(first),
                                 xr_semantic_plan_fingerprint(decoded)));

    size_t first_dump_size = 0;
    size_t decoded_dump_size = 0;
    char *first_dump = dump_plan(first, &first_dump_size);
    char *decoded_dump = dump_plan(decoded, &decoded_dump_size);
    REQUIRE(first_dump_size == decoded_dump_size);
    REQUIRE(memcmp(first_dump, decoded_dump, first_dump_size) == 0);

    uint8_t *roundtrip = NULL;
    size_t roundtrip_size = 0;
    REQUIRE(xr_xsm_encode(decoded, &roundtrip, &roundtrip_size, error, sizeof(error)));
    REQUIRE(roundtrip_size == first_size);
    REQUIRE(memcmp(roundtrip, first_bytes, first_size) == 0);

    xr_free(decoded_dump);
    xr_free(first_dump);
    xr_free(roundtrip);
    xr_semantic_plan_free(decoded);
    xr_free(second_bytes);
    xr_free(first_bytes);
    xr_semantic_plan_free(second);
    xr_semantic_plan_free(first);
}

static void test_explicit_panic_edge_and_roundtrip(void) {
    XrSemanticPlan *plan = build_panic_edge_plan();
    REQUIRE(xr_semantic_plan_edge_count(plan) == 2);
    const XrSemanticEdgeRecord *normal = NULL;
    const XrSemanticEdgeRecord *panic = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_edge_count(plan); i++) {
        const XrSemanticEdgeRecord *edge = xr_semantic_plan_edge(plan, i);
        REQUIRE(edge != NULL);
        if (edge->kind == XR_SEM_EDGE_NORMAL)
            normal = edge;
        if (edge->kind == XR_SEM_EDGE_PANIC)
            panic = edge;
    }
    REQUIRE(normal != NULL && panic != NULL);
    REQUIRE(normal->from_block == 0 && normal->to_block == 1);
    REQUIRE(normal->operation == XR_SEMANTIC_INDEX_NONE);
    REQUIRE(panic->from_block == 0 && panic->to_block == 2);
    REQUIRE(panic->operation < xr_semantic_plan_operation_count(plan));
    REQUIRE(xr_semantic_plan_operation(plan, panic->operation)->opcode == XI_TRY);

    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(xr_semantic_plan_edge_count(decoded) == 2);
    REQUIRE(xr_fingerprint_equal(xr_semantic_plan_fingerprint(plan),
                                 xr_semantic_plan_fingerprint(decoded)));
    xr_semantic_plan_free(decoded);
    xr_free(bytes);
    xr_semantic_plan_free(plan);
}

static void test_explicit_error_edge(void) {
    XrSemanticPlan *plan = build_error_edge_plan();
    REQUIRE(xr_semantic_plan_edge_count(plan) == 2);
    const XrSemanticEdgeRecord *error_edge = NULL;
    const XrSemanticEdgeRecord *normal_edge = NULL;
    for (uint32_t i = 0; i < xr_semantic_plan_edge_count(plan); i++) {
        const XrSemanticEdgeRecord *edge = xr_semantic_plan_edge(plan, i);
        REQUIRE(edge != NULL);
        if (edge->kind == XR_SEM_EDGE_ERROR)
            error_edge = edge;
        if (edge->kind == XR_SEM_EDGE_NORMAL)
            normal_edge = edge;
    }
    REQUIRE(error_edge != NULL && normal_edge != NULL);
    REQUIRE(error_edge->from_block == 0 && error_edge->to_block == 1);
    REQUIRE(error_edge->operation < xr_semantic_plan_operation_count(plan));
    REQUIRE(xr_semantic_plan_operation(plan, error_edge->operation)->opcode == XI_ERR_CHECK);
    REQUIRE(normal_edge->from_block == 0 && normal_edge->to_block == 2);
    xr_semantic_plan_free(plan);
}

static void test_typed_call_operand_contract(void) {
    XrSemanticPlan *plan = build_typed_call_operand_plan();
    const XrSemanticOperationRecord *call = NULL;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].opcode == XI_CALL) {
            call = &plan->operations[i];
            break;
        }
    }
    REQUIRE(call != NULL && call->operand_count == 3);
    XrSemanticOperandRecord *callee = &plan->operands[call->operand_begin];
    XrSemanticOperandRecord *first = &plan->operands[call->operand_begin + 1];
    XrSemanticOperandRecord *second = &plan->operands[call->operand_begin + 2];
    REQUIRE(callee->role == XR_SEM_OPERAND_CALLEE && callee->parameter == -1);
    REQUIRE((callee->flags & XR_SEM_OPERAND_CALL_CONTRACT) == 0);
    REQUIRE(first->role == XR_SEM_OPERAND_ARGUMENT && first->parameter == 0);
    REQUIRE(first->parameter_mode == XR_PARAM_REF && first->access == XR_CALL_ARG_REF);
    REQUIRE(first->origin == XI_PLACE_ORIGIN_STACK_LOCAL);
    REQUIRE(first->lifetime == XI_PLACE_LIFETIME_CALL_BOUND);
    REQUIRE((first->flags & (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE)) ==
            (XR_SEM_OPERAND_CALL_CONTRACT | XR_SEM_OPERAND_ADDRESSABLE));
    REQUIRE(second->role == XR_SEM_OPERAND_ARGUMENT && second->parameter == 1);
    REQUIRE(second->parameter_mode == XR_PARAM_MOVE && second->access == XR_CALL_ARG_MOVE);

    uint32_t saved_type = first->type;
    first->type = callee->type;
    expect_verify_failure(plan, "XR_SEM_0015");
    first->type = saved_type;
    uint8_t saved_role = first->role;
    first->role = XR_SEM_OPERAND_RECEIVER;
    expect_verify_failure(plan, "XR_SEM_0018");
    first->role = saved_role;

    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    const XrSemanticOperationRecord *decoded_call = NULL;
    for (uint32_t i = 0; i < decoded->operation_count; i++) {
        if (decoded->operations[i].opcode == XI_CALL) {
            decoded_call = &decoded->operations[i];
            break;
        }
    }
    REQUIRE(decoded_call != NULL);
    const XrSemanticOperandRecord *decoded_first =
        &decoded->operands[decoded_call->operand_begin + 1];
    REQUIRE(decoded_first->type == saved_type && decoded_first->parameter == 0);
    REQUIRE(decoded_first->parameter_mode == XR_PARAM_REF &&
            decoded_first->access == XR_CALL_ARG_REF);
    xr_semantic_plan_free(decoded);
    xr_free(bytes);
    xr_semantic_plan_free(plan);
}

static void test_parameter_and_capture_contracts(void) {
    XrSemanticPlan *parameters = build_owned_parameter_plan();
    REQUIRE(xr_semantic_plan_parameter_count(parameters) == 1);
    const XrSemanticParameterRecord *parameter = xr_semantic_plan_parameter(parameters, 0);
    REQUIRE(parameter != NULL && parameter->function == 0 && parameter->ordinal == 0);
    REQUIRE(parameter->type < xr_semantic_plan_type_count(parameters));
    REQUIRE(parameter->value < parameters->functions[0].value_count);
    REQUIRE(parameter->canonical_key != NULL);
    uint16_t saved_ordinal = parameters->parameters[0].ordinal;
    parameters->parameters[0].ordinal = 1;
    expect_verify_failure(parameters, "XR_SEM_0013");
    parameters->parameters[0].ordinal = saved_ordinal;
    xr_semantic_plan_free(parameters);

    XrSemanticPlan *captures = build_capture_contract_plan();
    REQUIRE(xr_semantic_plan_function_count(captures) == 3);
    REQUIRE(xr_semantic_plan_capture_count(captures) == 2);
    const XrSemanticFunctionRecord *child = xr_semantic_plan_function(captures, 1);
    const XrSemanticCaptureRecord *capture = xr_semantic_plan_capture(captures, 0);
    REQUIRE(child != NULL && child->parent == 0 && child->capture_begin == 0 &&
            child->capture_count == 1);
    REQUIRE(capture != NULL && capture->function == 1 && capture->source_function == 0);
    REQUIRE(capture->source == XR_SEM_CAPTURE_LOCAL_VALUE &&
            capture->source_capture == XR_SEMANTIC_INDEX_NONE);
    REQUIRE(capture->source_value == capture->source_index);
    REQUIRE(capture->kind == XR_SEM_CAPTURE_BY_COPY && capture->source_type == capture->type &&
            capture->storage_domain == XR_STORAGE_EXEC_LOCAL &&
            capture->value_capability == XR_SEM_VALUE_CONST);
    const XrSemanticCaptureRecord *transitive = xr_semantic_plan_capture(captures, 1);
    REQUIRE(transitive != NULL && transitive->function == 2 && transitive->source_function == 1);
    REQUIRE(transitive->source == XR_SEM_CAPTURE_PARENT_CAPTURE &&
            transitive->source_value == XR_SEMANTIC_INDEX_NONE && transitive->source_capture == 0 &&
            transitive->source_index == 0 && transitive->source_type == capture->source_type);

    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(captures, &bytes, &size, error, sizeof(error)));
    XrSemanticPlan *decoded = NULL;
    REQUIRE(xr_xsm_decode(bytes, size, &decoded, error, sizeof(error)));
    REQUIRE(xr_semantic_plan_capture_count(decoded) == 2);
    REQUIRE(strcmp(xr_semantic_plan_capture(decoded, 0)->name, "captured") == 0);
    xr_semantic_plan_free(decoded);
    xr_free(bytes);

    XrSemanticCaptureRecord *mutable_capture = &captures->captures[0];
    uint32_t saved_source = mutable_capture->source_value;
    mutable_capture->source_value = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(captures, "XR_SEM_0018");
    mutable_capture->source_value = saved_source;
    xr_semantic_plan_free(captures);
}

static void test_attachment_freezes_exact_function_identity(void) {
    XiFunc *root = xi_func_new("attachment_root", &stub_int);
    XiFunc *child = xi_func_new("attachment_child", &stub_int);
    XiFunc *grandchild = xi_func_new("attachment_grandchild", &stub_int);
    REQUIRE(root != NULL && child != NULL && grandchild != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    XiBlock *grandchild_entry = xi_block_new(grandchild);
    REQUIRE(root_entry != NULL && child_entry != NULL && grandchild_entry != NULL);
    xi_block_set_return(root_entry, xi_const_int(root, root_entry, 1, &stub_int));
    xi_block_set_return(child_entry, xi_const_int(child, child_entry, 2, &stub_int));
    xi_block_set_return(grandchild_entry, xi_const_int(grandchild, grandchild_entry, 3, &stub_int));
    root->children = (XiFunc **) xr_malloc(sizeof(*root->children));
    child->children = (XiFunc **) xr_malloc(sizeof(*child->children));
    REQUIRE(root->children != NULL && child->children != NULL);
    root->children[0] = child;
    root->nchildren = root->children_cap = 1;
    child->children[0] = grandchild;
    child->nchildren = child->children_cap = 1;
    child->parent_func = root;
    grandchild->parent_func = child;
    root->stage = child->stage = grandchild->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    REQUIRE(xr_semantic_plan_build_and_attach(root, error, sizeof(error)));
    REQUIRE(root->semantic_plan == child->semantic_plan &&
            child->semantic_plan == grandchild->semantic_plan);
    REQUIRE(root->semantic_plan_function_index == 0);
    REQUIRE(child->semantic_plan_function_index == 1);
    REQUIRE(grandchild->semantic_plan_function_index == 2);
    REQUIRE(strcmp(xr_semantic_plan_function(root->semantic_plan, 0)->name, "attachment_root") ==
            0);
    REQUIRE(strcmp(xr_semantic_plan_function(child->semantic_plan, 1)->name, "attachment_child") ==
            0);
    REQUIRE(strcmp(xr_semantic_plan_function(grandchild->semantic_plan, 2)->name,
                   "attachment_grandchild") == 0);
    xi_func_free(root);
}

static void test_unknown_capture_contract_fails_closed(void) {
    XiFunc *root = xi_func_new("unknown_capture_root", &stub_int);
    XiFunc *child = xi_func_new("unknown_capture_child", &stub_int);
    REQUIRE(root != NULL && child != NULL);
    XiBlock *root_entry = xi_block_new(root);
    XiBlock *child_entry = xi_block_new(child);
    REQUIRE(root_entry != NULL && child_entry != NULL);
    XiValue *captured = xi_const_str(root, root_entry, "unknown", &stub_string);
    XiValue *root_result = xi_const_int(root, root_entry, 1, &stub_int);
    XiValue *child_result = xi_const_int(child, child_entry, 2, &stub_int);
    REQUIRE(captured != NULL && root_result != NULL && child_result != NULL);
    xi_block_set_return(root_entry, root_result);
    xi_block_set_return(child_entry, child_result);
    root->children = (XiFunc **) xr_malloc(sizeof(*root->children));
    REQUIRE(root->children != NULL);
    root->children[0] = child;
    root->nchildren = 1;
    root->children_cap = 1;
    child->parent_func = root;
    child->ncaptures = 1;
    child->captures[0].source = XI_CAPTURE_SRC_REG;
    child->captures[0].capture_kind = XI_CAPTURE_BY_COPY;
    child->captures[0].name = "unknown";
    child->captures[0].type = &stub_string;
    child->captures[0].value = captured;
    child->captures[0].storage_domain = XR_STORAGE_DOMAIN_UNKNOWN;
    child->captures[0].value_capability = XR_SEM_VALUE_CAPABILITY_UNKNOWN;
    root->stage = XI_STAGE_OPTIMIZED;
    child->stage = XI_STAGE_OPTIMIZED;

    XrSemanticPlan *plan = NULL;
    char error[512] = {0};
    REQUIRE(!xr_semantic_plan_build(root, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_SEM_0018", strlen("XR_SEM_0018")) == 0);
    xi_func_free(root);
}

static void test_xsm_fail_closed_mutations(void) {
    XrSemanticPlan *plan = build_probe_plan();
    uint8_t *bytes = NULL;
    size_t size = 0;
    char error[512] = {0};
    REQUIRE(xr_xsm_encode(plan, &bytes, &size, error, sizeof(error)));

    expect_decode_failure(bytes, XR_XSM_HEADER_SIZE - 1, "XR_ARTIFACT_2001");

    uint8_t *mutation = copy_bytes(bytes, size);
    mutation[0] ^= 0x80;
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2000");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    mutation[8] ^= 0x01;
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2000");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    mutation[XR_XSM_HEADER_SIZE] ^= 0x01;
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2002");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    mutation[56] ^= 0x01;
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2003");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    mutation[88] ^= 0x01;
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2002");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    size_t key_offset =
        find_bytes(mutation + XR_XSM_HEADER_SIZE, size - XR_XSM_HEADER_SIZE, "type-v2");
    REQUIRE(key_offset != SIZE_MAX);
    mutation[XR_XSM_HEADER_SIZE + key_offset + 4] = '\0';
    rewrite_payload_digest(mutation, size);
    expect_decode_failure(mutation, size, "XR_ARTIFACT_2001");
    xr_free(mutation);

    mutation = copy_bytes(bytes, size);
    size_t operation_count_offset = XR_XSM_HEADER_SIZE + 3u * sizeof(uint32_t);
    uint32_t excessive_operations = 10000000u;
    for (unsigned i = 0; i < sizeof(uint32_t); i++)
        mutation[operation_count_offset + i] = (uint8_t) (excessive_operations >> (i * 8));
    rewrite_payload_digest(mutation, size);
    expect_decode_failure(mutation, size, "XR_EXEC_5003");
    xr_free(mutation);

    uint8_t truncated = 0;
    expect_decode_failure(&truncated, XR_XSM_MAX_ARTIFACT_SIZE + 1u, "XR_EXEC_5003");

    xr_free(bytes);
    xr_semantic_plan_free(plan);
}

static void expect_verify_failure(XrSemanticPlan *plan, const char *code) {
    char error[512] = {0};
    REQUIRE(!xr_semantic_plan_verify(plan, error, sizeof(error)));
    REQUIRE(strncmp(error, code, strlen(code)) == 0);
}

static void test_semantic_and_ownership_mutations(void) {
    XrSemanticPlan *plan = build_owned_parameter_plan();
    REQUIRE(plan->operation_count >= 2);
    REQUIRE(plan->ownership != NULL);
    REQUIRE(plan->ownership->event_count >= 2);
    REQUIRE(plan->ownership->edge_state_count >= 1);

    plan->operation_registry_fingerprint.bytes[0] ^= 0x01;
    expect_verify_failure(plan, "XR_SEM_0017");
    plan->operation_registry_fingerprint.bytes[0] ^= 0x01;

    uint16_t saved_opcode = plan->operations[0].opcode;
    plan->operations[0].opcode = XI_OP_COUNT;
    expect_verify_failure(plan, "XR_SEM_0015");
    plan->operations[0].opcode = saved_opcode;

    XrStableId saved_id = plan->operations[1].id;
    const char *saved_key = plan->operations[1].canonical_key;
    plan->operations[1].id = plan->operations[0].id;
    plan->operations[1].canonical_key = plan->operations[0].canonical_key;
    expect_verify_failure(plan, "XR_SEM_0003");
    plan->operations[1].id = saved_id;
    plan->operations[1].canonical_key = saved_key;

    XrOwnershipEventRecord *release_event = NULL;
    for (uint32_t i = 0; i < plan->ownership->event_count; i++) {
        if (plan->ownership->events[i].kind == XR_OWN_EVENT_RELEASE) {
            release_event = &plan->ownership->events[i];
            break;
        }
    }
    REQUIRE(release_event != NULL);
    int16_t saved_delta = release_event->logical_delta;
    release_event->logical_delta = 0;
    expect_verify_failure(plan, "XR_OWN_3001");
    release_event->logical_delta = saved_delta;

    uint8_t saved_reserved = release_event->reserved;
    release_event->reserved = 1;
    expect_verify_failure(plan, "XR_OWN_3002");
    release_event->reserved = saved_reserved;

    uint8_t saved_program_point = release_event->program_point;
    release_event->program_point = XR_OWN_POINT_EDGE;
    expect_verify_failure(plan, "XR_OWN_3002");
    release_event->program_point = saved_program_point;

    XrOwnershipEventRecord *opening_event = NULL;
    for (uint32_t i = 0; i < plan->ownership->event_count; i++) {
        XrOwnershipEventRecord *candidate = &plan->ownership->events[i];
        if (candidate->owner == release_event->owner && candidate->block == release_event->block &&
            candidate->successor == XR_SEMANTIC_INDEX_NONE && candidate->logical_delta > 0) {
            opening_event = candidate;
            break;
        }
    }
    REQUIRE(opening_event != NULL);
    REQUIRE(opening_event->program_point == XR_OWN_POINT_AFTER_OPERATION);
    uint8_t opening_program_point = opening_event->program_point;
    opening_event->program_point = XR_OWN_POINT_BLOCK_EXIT;
    expect_verify_failure(plan, "XR_OWN_3002");
    opening_event->program_point = opening_program_point;
    int16_t opening_delta = opening_event->logical_delta;
    opening_event->logical_delta = saved_delta;
    release_event->logical_delta = opening_delta;
    expect_verify_failure(plan, "XR_OWN_3003");
    opening_event->logical_delta = opening_delta;
    release_event->logical_delta = saved_delta;

    XrOwnershipEdgeStateRecord *edge = &plan->ownership->edge_states[0];
    int32_t saved_exit = edge->exit_balance;
    edge->exit_balance = saved_exit + 1;
    expect_verify_failure(plan, "XR_OWN_3001");
    edge->exit_balance = saved_exit;

    uint32_t saved_line = plan->operations[0].source_line;
    plan->operations[0].source_line = saved_line + 1;
    expect_verify_failure(plan, "XR_SEM_0004");
    uint8_t *invalid_artifact = NULL;
    size_t invalid_artifact_size = 0;
    char encode_error[512] = {0};
    REQUIRE(!xr_xsm_encode(plan, &invalid_artifact, &invalid_artifact_size, encode_error,
                           sizeof(encode_error)));
    REQUIRE(invalid_artifact == NULL && invalid_artifact_size == 0);
    REQUIRE(strncmp(encode_error, "XR_SEM_0004", strlen("XR_SEM_0004")) == 0);
    plan->operations[0].source_line = saved_line;

    char error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(plan, error, sizeof(error)));
    xr_semantic_plan_free(plan);

    plan = build_probe_plan();
    XrSemanticConstantRecord *boolean = NULL;
    for (uint32_t i = 0; i < plan->constant_count; i++) {
        if (plan->constants[i].kind == XR_SEM_CONST_BOOL)
            boolean = &plan->constants[i];
    }
    REQUIRE(boolean != NULL);
    boolean->integer = 2;
    expect_verify_failure(plan, "XR_SEM_0009");
    xr_semantic_plan_free(plan);

    plan = build_panic_edge_plan();
    XrSemanticEdgeRecord *panic = NULL;
    for (uint32_t i = 0; i < plan->edge_count; i++) {
        if (plan->edges[i].kind == XR_SEM_EDGE_PANIC)
            panic = &plan->edges[i];
    }
    REQUIRE(panic != NULL);
    panic->operation = XR_SEMANTIC_INDEX_NONE;
    expect_verify_failure(plan, "XR_SEM_0010");
    xr_semantic_plan_free(plan);

    plan = build_phi_dominance_plan();
    XrSemanticOperationRecord *phi = NULL;
    uint32_t left_value = XR_SEMANTIC_INDEX_NONE;
    uint32_t right_value = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        XrSemanticOperationRecord *operation = &plan->operations[i];
        if (operation->opcode == XI_PHI)
            phi = operation;
        else if (operation->block == 1)
            left_value = operation->result_value;
        else if (operation->block == 2)
            right_value = operation->result_value;
    }
    REQUIRE(phi != NULL && left_value != XR_SEMANTIC_INDEX_NONE &&
            right_value != XR_SEMANTIC_INDEX_NONE && phi->operand_count == 2);
    uint32_t saved_operand = plan->operands[phi->operand_begin].value;
    REQUIRE(saved_operand == left_value);
    plan->operands[phi->operand_begin].value = right_value;
    expect_verify_failure(plan, "XR_SEM_0016");
    plan->operands[phi->operand_begin].value = saved_operand;
    char graph_error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(plan, graph_error, sizeof(graph_error)));
    XrSemanticGraph graph = {0};
    REQUIRE(xr_semantic_graph_build(plan, &graph, graph_error, sizeof(graph_error)));
    REQUIRE(xr_semantic_graph_postdominates(&graph, phi->block, 0));
    REQUIRE(xr_semantic_graph_postdominates(&graph, phi->block, 1));
    REQUIRE(xr_semantic_graph_postdominates(&graph, phi->block, 2));
    REQUIRE(!xr_semantic_graph_postdominates(&graph, 1, 0));
    xr_semantic_graph_dispose(&graph);
    xr_semantic_plan_free(plan);
}

static void test_stack_extent_is_logical_and_fail_closed(void) {
    XiFunc *function = xi_func_new("stack_extent_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    XiValue *stack = xi_value_new(function, entry, XI_STACK_ALLOC, &stub_array, 0);
    REQUIRE(stack != NULL);
    stack->aux_int = XI_ARRAY_NEW;
    XiValue *result = xi_const_int(function, entry, 1, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(entry, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "stack extent plan build failed: %s\n", error);
    REQUIRE(built);
    const XrOwnershipCertificate *ownership = xr_semantic_plan_ownership(plan);
    REQUIRE(ownership != NULL);
    bool saw_alloc = false;
    bool saw_destroy = false;
    bool saw_physical_release = false;
    for (uint32_t i = 0; i < xr_ownership_certificate_event_count(ownership); i++) {
        const XrOwnershipEventRecord *event = xr_ownership_certificate_event(ownership, i);
        REQUIRE(event != NULL);
        saw_alloc |= event->kind == XR_OWN_EVENT_ALLOC && event->logical_delta == 1;
        saw_destroy |= event->kind == XR_OWN_EVENT_DESTROY && event->logical_delta == -1;
        saw_physical_release |= event->kind == XR_OWN_EVENT_RELEASE;
    }
    REQUIRE(saw_alloc && saw_destroy && !saw_physical_release);
    xr_semantic_plan_free(plan);
    xi_func_free(function);

    function = xi_func_new("stack_extent_escape_probe", &stub_array);
    REQUIRE(function != NULL);
    entry = xi_block_new(function);
    REQUIRE(entry != NULL);
    stack = xi_value_new(function, entry, XI_STACK_ALLOC, &stub_array, 0);
    REQUIRE(stack != NULL);
    stack->aux_int = XI_ARRAY_NEW;
    xi_block_set_return(entry, stack);
    function->stage = XI_STAGE_OPTIMIZED;
    plan = NULL;
    memset(error, 0, sizeof(error));
    REQUIRE(!xr_semantic_plan_build(function, &plan, error, sizeof(error)));
    REQUIRE(plan == NULL);
    REQUIRE(strncmp(error, "XR_OWN_3001", strlen("XR_OWN_3001")) == 0);
    xi_func_free(function);
}

static void test_loop_redefinition_closes_previous_owner(void) {
    XiFunc *function = xi_func_new("loop_redefinition_probe", &stub_int);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *header = xi_block_new(function);
    XiBlock *body = xi_block_new(function);
    XiBlock *latch = xi_block_new(function);
    XiBlock *exit = xi_block_new(function);
    REQUIRE(entry != NULL && header != NULL && body != NULL && latch != NULL && exit != NULL);

    xi_block_set_jump(entry, header);
    XiValue *condition = xi_const_bool(function, header, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(header, condition, body, exit);
    XiValue *capacity = xi_const_int(function, body, 1, &stub_int);
    REQUIRE(capacity != NULL);
    XiValue *array = xi_value_new(function, body, XI_ARRAY_NEW, &stub_array, 1);
    REQUIRE(array != NULL);
    array->args[0] = capacity;
    array->flags = xi_op_default_effects(XI_ARRAY_NEW);
    xi_block_set_jump(body, latch);
    xi_block_set_jump(latch, header);
    XiValue *result = xi_const_int(function, exit, 0, &stub_int);
    REQUIRE(result != NULL);
    xi_block_set_return(exit, result);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "loop-redefinition plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL && plan->ownership != NULL);

    uint32_t origin_block = XR_SEMANTIC_INDEX_NONE;
    for (uint32_t i = 0; i < plan->operation_count; i++) {
        if (plan->operations[i].opcode == XI_ARRAY_NEW) {
            origin_block = plan->operations[i].block;
            break;
        }
    }
    REQUIRE(origin_block != XR_SEMANTIC_INDEX_NONE);
    bool saw_redefinition_release = false;
    for (uint32_t i = 0; i < plan->ownership->event_count; i++) {
        const XrOwnershipEventRecord *event = &plan->ownership->events[i];
        saw_redefinition_release |= event->kind == XR_OWN_EVENT_RELEASE &&
                                    event->logical_delta == -1 && event->successor == origin_block;
    }
    REQUIRE(saw_redefinition_release);
    xr_semantic_plan_free(plan);
    xi_func_free(function);
}

static void test_nullable_borrowed_parameter_keeps_sealed_provenance(void) {
    XiFunc *function = xi_func_new("nullable_borrowed_parameter_probe", &stub_array);
    REQUIRE(function != NULL);
    XiBlock *entry = xi_block_new(function);
    XiBlock *null_return = xi_block_new(function);
    XiBlock *parameter_return = xi_block_new(function);
    REQUIRE(entry != NULL && null_return != NULL && parameter_return != NULL);
    XiValue *parameter = xi_param(function, entry, 0, &stub_array);
    REQUIRE(parameter != NULL);
    function->nparams = 1;
    function->params = (XiValue **) xr_malloc(sizeof(*function->params));
    REQUIRE(function->params != NULL);
    function->params[0] = parameter;
    function->arc_return_ownership.kind = XI_RETURN_OWNERSHIP_BORROWED_PARAM;
    function->arc_return_ownership.param_index = 0;
    function->arc_return_ownership.complete = true;

    XiValue *condition = xi_const_bool(function, entry, true, &stub_bool);
    REQUIRE(condition != NULL);
    xi_block_set_if(entry, condition, null_return, parameter_return);
    XiValue *null_value = xi_const_null(function, null_return, &stub_null);
    REQUIRE(null_value != NULL);
    xi_block_set_return(null_return, null_value);
    xi_block_set_return(parameter_return, parameter);
    xi_arc_analyze_contracts(function);
    function->stage = XI_STAGE_OPTIMIZED;

    char error[512] = {0};
    XrSemanticPlan *plan = NULL;
    bool built = xr_semantic_plan_build(function, &plan, error, sizeof(error));
    if (!built)
        fprintf(stderr, "nullable-borrow plan build failed: %s\n", error);
    REQUIRE(built && plan != NULL);
    REQUIRE(plan->functions[0].return_provenance == XR_SEM_RETURN_BORROWED_PARAM);
    REQUIRE(plan->functions[0].return_parameter == 0);
    xr_semantic_plan_free(plan);
    xi_func_free(function);
}

int main(void) {
    test_stable_ids();
    test_operation_registry();
    test_immutable_owned_snapshot();
    test_xsm_roundtrip_and_determinism();
    test_explicit_panic_edge_and_roundtrip();
    test_explicit_error_edge();
    test_typed_call_operand_contract();
    test_parameter_and_capture_contracts();
    test_attachment_freezes_exact_function_identity();
    test_unknown_capture_contract_fails_closed();
    test_xsm_fail_closed_mutations();
    test_semantic_and_ownership_mutations();
    test_stack_extent_is_logical_and_fail_closed();
    test_loop_redefinition_closes_previous_owner();
    test_nullable_borrowed_parameter_keeps_sealed_provenance();
    printf("SemanticPlan/XSM tests passed\n");
    return 0;
}
