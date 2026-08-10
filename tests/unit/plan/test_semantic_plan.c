/*
 * test_semantic_plan.c - Immutable SemanticPlan and exact-version XSM contract
 */

#include "../../../src/base/xmalloc.h"
#include "../../../src/base/xsha256.h"
#include "../../../src/ir/xi.h"
#include "../../../src/ir/xi_arc.h"
#include "../../../src/ir/xi_effect.h"
#include "../../../src/plan/format/xr_xsm_schema.h"
#include "../../../src/plan/ownership/xr_ownership_certificate.h"
#include "../../../src/plan/ownership/xr_ownership_certificate_internal.h"
#include "../../../src/plan/semantic/xr_semantic_builder.h"
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
    uint32_t saved_operand = plan->operands[phi->operand_begin];
    REQUIRE(saved_operand == left_value);
    plan->operands[phi->operand_begin] = right_value;
    expect_verify_failure(plan, "XR_SEM_0016");
    plan->operands[phi->operand_begin] = saved_operand;
    char graph_error[512] = {0};
    REQUIRE(xr_semantic_plan_verify(plan, graph_error, sizeof(graph_error)));
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
    test_xsm_fail_closed_mutations();
    test_semantic_and_ownership_mutations();
    test_stack_extent_is_logical_and_fail_closed();
    test_loop_redefinition_closes_previous_owner();
    test_nullable_borrowed_parameter_keeps_sealed_provenance();
    printf("SemanticPlan/XSM tests passed\n");
    return 0;
}
