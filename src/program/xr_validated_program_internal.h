#ifndef XR_VALIDATED_PROGRAM_INTERNAL_H
#define XR_VALIDATED_PROGRAM_INTERNAL_H

#include "../core/xr_core_spec_gen.h"
#include "xr_program_verify.h"

#include <stdatomic.h>

typedef struct XrValidatedVariant {
    uint16_t *payload_types;
    uint32_t payload_count;
    char *display_name;
} XrValidatedVariant;

typedef struct XrValidatedType {
    XrCoreIrKey key;
    uint16_t type_id;
    XrCoreIrTypeKind kind;
    XrCoreIrNominalKind nominal_kind;
    XrCoreIrTypeOwnership ownership;
    XrCoreIrCopyContract copy_contract;
    uint16_t *field_types;
    uint32_t field_count;
    XrValidatedVariant *variants;
    uint32_t variant_count;
    uint16_t view_element_type;
    uint16_t array_element_type;
    uint16_t atomic_element_type;
    XrStableId resource_id;
    XrCoreIrViewCapability view_capability;
    uint32_t signature_id;
    uint32_t interface_id;
    XrCoreIrInterfaceUseKind interface_use_kind;
    char *display_name;
} XrValidatedType;

typedef struct XrValidatedSignature {
    uint16_t *parameter_types;
    XrParamMode *parameter_modes;
    uint32_t parameter_count;
    bool has_receiver;
    XrParamMode receiver_mode;
    uint16_t result_type_id;
    XrCoreIrOwnershipDisposition result_ownership;
    XrViewOrigin *result_borrow_origins;
    uint32_t result_borrow_origin_count;
    uint16_t error_type_id;
    uint16_t panic_type_id;
    uint32_t effect_mask;
    uint32_t capability_mask;
} XrValidatedSignature;

typedef struct XrValidatedInterface {
    XrCoreIrKey key;
    uint32_t *slot_signature_ids;
    uint32_t slot_count;
} XrValidatedInterface;

typedef struct XrValidatedConformance {
    XrCoreIrKey key;
    uint16_t implementor_type_id;
    XrCoreIrNominalKind implementor_kind;
    uint32_t interface_id;
    uint32_t *slot_function_ids;
    uint32_t slot_count;
} XrValidatedConformance;

typedef struct XrValidatedProviderRequirement {
    XrStableId contract_id;
    XrProgramProviderOperationRequirement *operations;
    uint32_t operation_count;
} XrValidatedProviderRequirement;

/* The public CoreSpec has one typed provider-call operation.  These are
 * execution-private
 * logical call shapes selected from the validated operand
 * and result types; they are not
 * serialized opcodes or provider C ABIs. */
typedef enum XrProviderLogicalCallKind {
    XR_PROVIDER_LOGICAL_CALL_INVALID = 0,
    XR_PROVIDER_LOGICAL_CALL_I64_UNARY,
    XR_PROVIDER_LOGICAL_CALL_I64_NULLARY,
    XR_PROVIDER_LOGICAL_CALL_BOOL_I64_UNARY,
    XR_PROVIDER_LOGICAL_CALL_OPTIONAL_I64_PAIR_NULLARY,
} XrProviderLogicalCallKind;

typedef struct XrValidatedRoot {
    XrCoreIrRootKind kind;
    uint32_t parameter_ordinal;
    uint32_t source_value_id;
} XrValidatedRoot;

typedef struct XrValidatedValueRootSet {
    uint32_t *root_ids;
    uint32_t root_count;
} XrValidatedValueRootSet;

typedef struct XrValidatedCoroutineState {
    uint32_t continuation_block;
} XrValidatedCoroutineState;

typedef struct XrValidatedCoroutineSafepoint {
    uint32_t resume_state_id;
    uint32_t *live_value_ids;
    uint32_t live_value_count;
} XrValidatedCoroutineSafepoint;

/* String constants borrow their bytes from the validated program's own
 * artifact copy, so they live exactly as long as the program handle. */
typedef struct XrValidatedConstant {
    uint16_t type_id;
    XrCoreIrConstantKind kind;
    union {
        int64_t i64;
        uint64_t f64_bits;
        bool boolean;
        struct {
            const uint8_t *bytes;
            uint32_t size;
        } string;
        uint32_t rune;
    } value;
} XrValidatedConstant;

typedef struct XrValidatedInstruction {
    uint16_t operation_id;
    uint32_t result_id;
    uint16_t result_type_id;
    XrCoreIrValueCategory result_category;
    XrCoreIrOwnershipDisposition result_ownership;
    uint32_t *operands;
    uint32_t operand_count;
    XrCoreIrImmediateKind immediate_kind;
    union {
        int64_t i64;
        uint32_t u32;
        bool boolean;
        uint32_t constant_id;
        uint32_t function_id;
        struct {
            uint32_t module_index;
            uint32_t slot_index;
        } module_slot;
        uint32_t field_ordinal;
        uint32_t variant_ordinal;
        struct {
            uint32_t variant_ordinal;
            uint32_t field_ordinal;
        } variant_field;
        uint16_t type_id;
        struct {
            uint32_t requirement_index;
            uint32_t operation_index;
        } provider_operation;
        struct {
            uint32_t function_id;
            uint32_t safepoint_id;
        } coroutine_call;
        struct {
            uint32_t safepoint_id;
            uint16_t request_kind;
            uint16_t request_operand_count;
        } coroutine_suspend;
    } immediate;
    uint32_t *successors;
    uint32_t successor_count;
} XrValidatedInstruction;

typedef struct XrValidatedBlock {
    uint32_t *argument_ids;
    uint16_t *argument_types;
    XrCoreIrValueCategory *argument_categories;
    XrCoreIrOwnershipDisposition *argument_ownerships;
    uint32_t argument_count;
    XrValidatedInstruction *instructions;
    uint32_t instruction_count;
} XrValidatedBlock;

typedef struct XrValidatedFunction {
    uint32_t signature_id;
    uint16_t *parameter_types;
    XrParamMode *parameter_modes;
    uint32_t parameter_count;
    bool has_receiver;
    XrParamMode receiver_mode;
    uint16_t result_type_id;
    XrCoreIrOwnershipDisposition result_ownership;
    XrViewOrigin *result_borrow_origins;
    uint32_t result_borrow_origin_count;
    uint16_t error_type_id;
    uint16_t panic_type_id;
    uint32_t effect_mask;
    uint32_t capability_mask;
    uint32_t entry_block;
    XrValidatedBlock *blocks;
    uint32_t block_count;
    uint16_t *value_types;
    XrCoreIrValueCategory *value_categories;
    XrCoreIrOwnershipDisposition *value_ownerships;
    uint32_t *value_blocks;
    uint32_t *value_positions;
    XrValidatedRoot *roots;
    uint32_t root_count;
    XrValidatedValueRootSet *value_root_sets;
    XrValidatedCoroutineState *coroutine_states;
    uint32_t coroutine_state_count;
    XrValidatedCoroutineSafepoint *coroutine_safepoints;
    uint32_t coroutine_safepoint_count;
    uint32_t value_count;
    uint32_t flags;
    /* Zero denotes a function outside the runtime module table. */
    uint32_t module_index_plus_one;
} XrValidatedFunction;

typedef struct XrValidatedModuleSlot {
    XrCoreIrKey key;
    uint16_t type_id;
    uint32_t flags;
} XrValidatedModuleSlot;

typedef struct XrValidatedModule {
    XrCoreIrKey key;
    uint32_t initializer;
    uint32_t *dependencies;
    uint32_t dependency_count;
    XrValidatedModuleSlot *slots;
    uint32_t slot_count;
} XrValidatedModule;

struct XrValidatedProgram {
    atomic_uint_least32_t references;
    uint8_t *bytes;
    size_t size;
    XrProgramId id;
    uint8_t semantic_profile_fingerprint[XR_PROGRAM_DIGEST_SIZE];
    XrValidatedType *types;
    uint32_t type_count;
    XrValidatedSignature *signatures;
    uint32_t signature_count;
    XrValidatedInterface *interfaces;
    uint32_t interface_count;
    XrValidatedConformance *conformances;
    uint32_t conformance_count;
    XrValidatedProviderRequirement *provider_requirements;
    uint32_t provider_requirement_count;
    XrValidatedConstant *constants;
    uint32_t constant_count;
    XrValidatedFunction *functions;
    uint32_t function_count;
    /* Dense module indices are the immutable initialization order. */
    XrValidatedModule *modules;
    uint32_t module_count;
    uint32_t module_slot_count;
    uint32_t entry_function;
    uint64_t verifier_work;
};

static inline bool xr_program_type_id_is_dynamic(uint16_t type_id) {
    return type_id >= XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
}

static inline const XrValidatedType *xr_validated_program_type(const XrValidatedProgram *program,
                                                               uint16_t type_id) {
    if (!program || !xr_program_type_id_is_dynamic(type_id))
        return NULL;
    uint32_t index = (uint32_t) type_id - XR_CORE_PROGRAM_TYPE_DYNAMIC_BASE;
    return index < program->type_count ? &program->types[index] : NULL;
}

static inline XrCoreIrTypeOwnership
xr_validated_program_type_ownership(const XrValidatedProgram *program, uint16_t type_id) {
    const XrProgramBuiltinTypeRow *builtin = xr_program_builtin_type_row(type_id);
    if (builtin)
        return builtin->ownership;
    const XrValidatedType *type = xr_validated_program_type(program, type_id);
    return type ? type->ownership : XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL;
}

static inline XrCoreIrCopyContract
xr_validated_program_copy_contract(const XrValidatedProgram *program, uint16_t type_id) {
    const XrProgramBuiltinTypeRow *builtin = xr_program_builtin_type_row(type_id);
    if (builtin)
        return builtin->copy_contract;
    const XrValidatedType *type = xr_validated_program_type(program, type_id);
    return type ? type->copy_contract : XR_CORE_IR_COPY_TRIVIAL;
}

/* A `string` owner is pure immutable memory: it has no finalizer, holds no
 * resource and its release is never observable, so an implicit trap exit may
 * leave it for executor-private reclamation.  Every other affine owner must
 * be explicitly cleaned up before an operation that can trap. */
static inline bool xr_validated_program_owner_needs_cleanup_before_trap(uint16_t type_id) {
    return type_id != XR_CORE_TYPE_STRING;
}

/* Bit mask over builtin type ids that core.output.group renders. */
static inline bool xr_validated_program_type_is_displayable(uint16_t type_id) {
    return type_id < 32u &&
           ((XR_CORE_OPERAND_DOMAIN_CORE_OUTPUT_GROUP >> type_id) & UINT32_C(1)) != 0u;
}

static inline bool xr_validated_program_type_is_optional_i64_pair(const XrValidatedProgram *program,
                                                                  uint16_t type_id,
                                                                  uint16_t *pair_type_id_out) {
    const XrValidatedType *optional = xr_validated_program_type(program, type_id);
    if (!optional || optional->kind != XR_CORE_IR_TYPE_VARIANT ||
        optional->nominal_kind != XR_CORE_IR_NOMINAL_NONE ||
        optional->ownership != XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL ||
        optional->copy_contract != XR_CORE_IR_COPY_TRIVIAL || optional->variant_count != 2u ||
        optional->variants[0].payload_count != 0u || optional->variants[1].payload_count != 1u ||
        !optional->variants[1].payload_types)
        return false;
    uint16_t pair_type_id = optional->variants[1].payload_types[0];
    const XrValidatedType *pair = xr_validated_program_type(program, pair_type_id);
    if (!pair || pair->kind != XR_CORE_IR_TYPE_AGGREGATE ||
        pair->nominal_kind != XR_CORE_IR_NOMINAL_NONE ||
        pair->ownership != XR_CORE_IR_TYPE_OWNERSHIP_TRIVIAL ||
        pair->copy_contract != XR_CORE_IR_COPY_TRIVIAL || pair->field_count != 2u ||
        !pair->field_types || pair->field_types[0] != XR_CORE_TYPE_I64 ||
        pair->field_types[1] != XR_CORE_TYPE_I64)
        return false;
    if (pair_type_id_out)
        *pair_type_id_out = pair_type_id;
    return true;
}

static inline XrProviderLogicalCallKind
xr_validated_program_provider_call_kind(const XrValidatedProgram *program, uint16_t result_type_id,
                                        const uint16_t *operand_types, uint32_t operand_count) {
    if (result_type_id == XR_CORE_TYPE_I64) {
        if (operand_count == 0u)
            return XR_PROVIDER_LOGICAL_CALL_I64_NULLARY;
        if (operand_count == 1u && operand_types && operand_types[0] == XR_CORE_TYPE_I64)
            return XR_PROVIDER_LOGICAL_CALL_I64_UNARY;
    }
    if (result_type_id == XR_CORE_TYPE_BOOL && operand_count == 1u && operand_types &&
        operand_types[0] == XR_CORE_TYPE_I64)
        return XR_PROVIDER_LOGICAL_CALL_BOOL_I64_UNARY;
    if (operand_count == 0u &&
        xr_validated_program_type_is_optional_i64_pair(program, result_type_id, NULL))
        return XR_PROVIDER_LOGICAL_CALL_OPTIONAL_I64_PAIR_NULLARY;
    return XR_PROVIDER_LOGICAL_CALL_INVALID;
}

/* Logical signatures are checked against Program types independently of an
 * executor's physical adapter inventory. The view is bounded prefix type data;
 * matching never allocates or retains borrowed contract storage. */
XR_FUNC bool xr_validated_program_provider_type_matches(const XrValidatedProgram *program,
                                                         uint16_t type_id,
                                                         XrProviderLogicalTypeView logical);

/* Canonical ownership transfer for a validated operand. The returned SSA owner
 * is consumed after
 * successful execution, or at callee entry for MOVE arguments.
 * calls_only restricts the query to
 * the latter case. No execution state is kept. */
XR_FUNC uint32_t xr_validated_instruction_consumed_owner(const XrValidatedProgram *program,
                                                         const XrValidatedFunction *function,
                                                         const XrValidatedInstruction *instruction,
                                                         uint32_t operand_index, uint32_t block_id,
                                                         bool calls_only);

/* Private query for an affine READ existential's exact owner. */
/* It follows canonical block arguments and explicit reborrow anchors. */
/* Missing or ambiguous ownership returns NONE. */
XR_FUNC uint32_t xr_validated_function_scoped_affine_borrow_owner(
    const XrValidatedProgram *program, const XrValidatedFunction *function, uint32_t value_id,
    uint32_t block_id);

/* Private query for a non-owning callable whose complete SSA provenance */
/* terminates at a frame parameter or captureless static callable pack. */
XR_FUNC bool xr_validated_function_frame_stable_callable(const XrValidatedProgram *program,
                                                         const XrValidatedFunction *function,
                                                         uint32_t value_id);

#endif /* XR_VALIDATED_PROGRAM_INTERNAL_H */
