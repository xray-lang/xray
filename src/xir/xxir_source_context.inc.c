/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_context.inc.c - One Source construction and its owned semantic units
 *
 * KEY CONCEPT:
 *   Actual receiving maps outlive the temporary checker before storage admission.
 */
typedef struct SourceFunctionValue {
    struct SourceFunctionValue *next;
    SourceName *function, *value;
} SourceFunctionValue;
typedef struct SourceHistoryCorrespondence {
    const struct XrXirSourceHistoryUnit *unit;
    uint32_t *modules, *functions, *slots;
    XrXirType *types;
    uint32_t module_count, function_count, slot_count, type_count;
} SourceHistoryCorrespondence;
struct SourceSemanticEnvironment {
    XrModuleGraph *graph;
    SourceName **names, *symbols, **latest;
    SourceName *implicit_result;
    uint32_t module_count, symbol_count, latest_count;
    uint32_t *binding_reads, *binding_writes, *binding_pushes;
    uint32_t slot_count;
    SourceHistoryCorrespondence history;
};
typedef struct XrXirSourceHistoryUnit {
    const struct XrXirSourceGenerations *owner;
    uint64_t serial;
    const SourceSemanticEnvironment *semantic;
    const XrXirArtifact *checked;
    XrXirProgram *program;
    uint32_t entry_module;
    bool published;
    bool interactive;
} SourceHistoryUnit;
typedef struct SourceHistoryEdge { uint32_t from,to; } SourceHistoryEdge;

typedef struct SourceGeneratedDebug {
    struct SourceGeneratedDebug *next;
    XrXirSourceDebugFunction function;
} SourceGeneratedDebug;
typedef struct SourceHistoryReceive {
    const SourceHistoryUnit *unit;
    struct SourceLibraryMap *map;
    SourceName **symbols, **latest;
    SourceName *implicit_result;
    uint32_t latest_count;
    SourceHistoryEdge *edges;
    uint32_t edge_count, edge_capacity;
} SourceHistoryReceive;
typedef struct SourceContext {
    XrModuleGraph *graph;
    XrXirLinkageKind linkage_kind;
    XrXirCompileContext compile;
    uint32_t remaining_blocks, remaining_instructions;
    XrXirSourceDiagnostic diagnostic;
    SourceMemory *memory,*retired_arrays;
    XrXirConstruction *construction;
    SourceSemanticEnvironment **semantic_output;
    const struct XrXirSourceHistoryUnit *history_input, *self_unit;
    struct SourceHistoryReceive *history;
    SourceLibraryUnit *library_units;
    SourceRootQuery *root_queries;
    SourceGeneratedDebug *generated_debug;
    uint32_t generated_debug_count;
    SourceRootFlow *root_flow;
    XrXirProvenance *effect_evidence;
    SourceManifest *manifests;
    XrXirFunction *functions;
    SourceFunction *bodies;
    XrXirSourceModule *modules;
    XrXirFunctionIdentity *identities;
    XrXirGeneric *generics;
    XrXirTypes types;
    XrXirNominalTable nominals;
    SourceTypeScope type_scope;
    XrXirInterfaceTable interfaces;
    XrXirImplementationTable implementations;
    XrXirDefaultTable defaults;
    uint32_t default_capacity;
    bool implementations_ready, declarations_building;
    SourceName **interface_sources, **interface_members;
    uint32_t **interface_member_declarations;
    uint32_t **nominal_members, **nominal_variants;
    uint32_t **nominal_defaults;
    SourceName **nominal_sources, **nominal_methods;
    bool *nominal_defaultable;
    uint32_t *nominal_constructors;
    uint32_t type_capacity;
    XrXirSlot *slots;
    XrXirLiteral *literals;
    SourceName **names, *locals, *scope;
    SourceFact *facts;
    SourceEpoch *epochs;
    uint64_t next_epoch;
    uint32_t flow_alternatives;
    uint32_t function_count, slot_count, literal_count, literal_capacity;
    uint32_t slot_capacity, function_value_capacity;
    uint32_t module_count, entry_function;
    bool core_factory, binding_readers;
    bool interactive, type_query;
    const char *module_scope;
    AstNode *repl_tail;
    SourceName *repl_result;
    SourceFunctionValue *function_values;
    uint32_t *binding_reads, *binding_writes, *binding_pushes;
    SourceName *assertion, *assert_panics, *assert_equal;
    uint32_t first_closure, next_closure, closure_limit, function_capacity;
    uint32_t function, module, depth;
    SourceComptimeSite *comptime_sites;
    const SourceInstanceBatch *instance_constants;
    const XrXirModule *literal_definition;
    const SourceComptimeSite *comptime_input;
    const XrXirTypes *comptime_input_types;
    SourceMaterialization *materializations,*materialization_tail;
    uint32_t materialization_begin,materialization_count;
    uint32_t comptime_capacity,comptime_count,comptime_matched;
    SourceExpressionPlan *active_expression;
    XrXirSourceDebugPoint debug_point;
    uint32_t debug_function;
    bool debug_entry, debug_jump_entry;
    SourceName **debug_names;
    uint32_t debug_name_capacity;
    /* This exact argument plan owns the pre-loss role, never its children. */
    SourceExpressionPlan *effect_argument;
    SourceLoop *loop;
    bool returned, has_generics, query_ready;
    XrXirSourceView query;
    XrXirSourceSyntaxView syntax;
    XrXirSourceCalls calls;
    uint32_t call_capacity;
    uint32_t syntax_declaration_capacity, syntax_reference_capacity, syntax_marker_capacity;
    uint32_t declaration_capacity, reference_capacity, expression_capacity;
    uint32_t prelude_module; /* One-based actual graph index, zero when unused. */
    uint32_t atomic_module, atomic_declaration, atomic_members[10];
    uint32_t task_module, task_declaration;
    uint32_t channel_module, channel_declaration, channel_members[10];
    uint32_t iterator_module, iterator_declaration, iterator_members[3];
    uint32_t query_module_capacity, array_module, array_declaration, array_members[XR_NATIVE_OPERATION_ARRAY_RESERVE + 1], length_declaration;
    uint32_t string_module, string_declaration, string_members[5];
    uint32_t f64_module, f64_declaration, f64_parse_member;
} SourceContext;
