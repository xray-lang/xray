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
typedef struct SourceHistoryCorrespondence {
    const struct XrXirSourceHistoryUnit *unit;
    uint32_t *modules, *functions, *slots;
    XrXirType *types;
    uint32_t module_count, function_count, slot_count, type_count;
} SourceHistoryCorrespondence;
struct SourceSemanticEnvironment {
    XrModuleGraph *graph;
    SourceName **names, *symbols, **latest;
    uint32_t module_count, symbol_count, latest_count;
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
} SourceHistoryUnit;
typedef struct SourceHistoryEdge { uint32_t from,to; } SourceHistoryEdge;
typedef struct SourceHistoryReceive {
    const SourceHistoryUnit *unit;
    struct SourceLibraryMap *map;
    SourceName **symbols, **latest;
    uint32_t latest_count;
    SourceHistoryEdge *edges;
    uint32_t edge_count, edge_capacity;
} SourceHistoryReceive;
typedef struct SourceInstanceBatch {
    const XirCtfeInstanceView *views;
    uint32_t *providers;
    uint32_t count;
} SourceInstanceBatch;
typedef struct SourceMaterialization SourceMaterialization;
typedef struct SourceDeriveJob SourceDeriveJob;
typedef struct SourceContext {
    XrModuleGraph *graph;
    XrXirLinkageKind linkage_kind;
    XrXirCompileContext compile;
    uint32_t remaining_blocks, remaining_instructions;
    XrXirSourceDiagnostic diagnostic;
    SourceMemory *memory;
    SourceMemory *retired_vectors;
    SourceOwnedTextEntry *owned_texts;
    uint32_t owned_text_count, owned_text_capacity;
    XrXirConstruction *construction;
    SourceSemanticEnvironment **semantic_output;
    const struct XrXirSourceHistoryUnit *history_input, *self_unit;
    struct SourceHistoryReceive *history;
    SourceLibraryUnit *library_units;
    SourceLibraryAdmission *library_admissions;
    XrXirDerivationTable derivations;
    SourceDeriveJob *derive_jobs;const SourceDeriveJob *derive_input;
    SourceRootQuery *root_queries;
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
    uint32_t module_count, entry_function;
    bool core_factory,derive_present;
    SourceName *assertion, *assert_panics, *assert_equal;
    uint32_t first_closure, next_closure, closure_limit, function_capacity;
    uint32_t function, module, depth;
    SourceComptimeSite *comptime_sites;
    const SourceInstanceBatch *instance_constants;
    const XrXirModule *literal_definition;
    const SourceComptimeSite *comptime_input;
    const XrXirTypes *comptime_input_types;
    uint32_t comptime_capacity,comptime_count,comptime_matched;
    SourceMaterialization *materializations,*materialization_tail;
    uint32_t materialization_begin,materialization_count;
    uint64_t comptime_output_bytes;
    SourceExpressionPlan *active_expression;
    /* This exact argument plan owns the pre-loss role, never its children. */
    SourceExpressionPlan *effect_argument;
    SourceLoop *loop;
    bool returned, has_generics, query_ready;
    XrXirSourceView query;
    XrXirSourceSyntaxView syntax;
    uint32_t syntax_declaration_capacity, syntax_reference_capacity, syntax_marker_capacity;
    uint32_t declaration_capacity, reference_capacity, expression_capacity;
    uint32_t prelude_module; /* One-based actual graph index, zero when unused. */
    uint32_t math_module;
    uint32_t atomic_module, atomic_declaration, atomic_members[10];
    uint32_t task_module, task_declaration;
    uint32_t iterator_module, iterator_declaration, iterator_members[3];
    uint32_t query_module_capacity, array_module, array_declaration, array_members[XR_NATIVE_OPERATION_COUNT], length_declaration;
    uint32_t string_module, string_declaration, string_members[6];
    uint32_t type_info_declaration,f64_module,f64_declaration,f64_parse_member;
} SourceContext;
