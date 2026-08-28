/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xa_typed_program.h"
#include "xa_node_table.h"
#include "xa_scalar_program_authority.h"
#include "xa_program_semantic_closure.h"
#include "xa_i64_overflow_program.h"
#include "xanalyzer.h"
#include "xanalyzer_symbol.h"
#include "../parser/xast_nodes.h"
#include "../../base/xmalloc.h"
#include "../../module/xmodule_graph.h"
#include "../../plan/semantic/xr_source_semantic_identity.h"

struct XaTypedProgram {
    const struct AstNode *syntax;
    struct XaAnalyzer *semantics;
    /* Compilation unit identity, snapshotted with the rest of the typed
     * program.  Lowering attributes plans to exact source and must read the
     * unit it was handed, not an analyzer cursor whose lifetime is the
     * analysis pass. */
    const char *source_file;
    const struct XgGlobalEvidence *global_evidence;
    uint64_t semantic_revision;
    uint32_t module_id;
    XrProgramSemanticModuleInput source_module_authority;
    bool source_module_authority_present;
    XaScalarProgramAuthority *scalar_authority;
    XrProgramSemanticClosure *program_semantic_closure;
    XaNodeConversionEntry *conversions;
    uint32_t conversion_count;
    bool verified;
};

static const XrModuleSpec *find_module_spec(const struct XaAnalyzer *analyzer,
                                            const struct AstNode *syntax) {
    const XrModuleGraph *graph = analyzer ? analyzer->graph : NULL;
    const XrModuleSpec *found = NULL;
    if (!graph || !syntax)
        return NULL;
    for (int i = 0; i < graph->spec_count; i++) {
        if (graph->specs[i].ast != syntax)
            continue;
        if (found)
            return NULL;
        found = &graph->specs[i];
    }
    return found;
}

static XaDiagnostic *first_error(struct XaAnalyzer *analyzer) {
    int count = 0;
    for (XaDiagnostic *diag = xa_analyzer_get_diagnostics(analyzer, &count); diag;
         diag = diag->next) {
        if (diag->severity == XR_DIAG_SEV_ERROR)
            return diag;
    }
    return NULL;
}

typedef enum OwnershipPublicationStatus {
    OWNERSHIP_PUBLICATION_OK = 0,
    OWNERSHIP_PUBLICATION_INVALID,
    OWNERSHIP_PUBLICATION_RESOURCE_FAILURE,
} OwnershipPublicationStatus;

static OwnershipPublicationStatus verify_scope_ownership(const XaScope *scope,
                                                         uint32_t *source_line) {
    if (!scope)
        return OWNERSHIP_PUBLICATION_OK;
    int symbol_count = 0;
    XaSymbol **symbols = xa_scope_get_all_symbols((XaScope *) scope, &symbol_count);
    if (!symbols && xa_scope_count_symbols((XaScope *) scope) != 0)
        return OWNERSHIP_PUBLICATION_RESOURCE_FAILURE;

    const uint32_t required_candidate = XA_OWNERSHIP_EV_BINDING_LIVE | XA_OWNERSHIP_EV_ROOT_UNIQUE |
                                        XA_OWNERSHIP_EV_LOAN_FREE | XA_OWNERSHIP_EV_ALIAS_FREE |
                                        XA_OWNERSHIP_EV_ESCAPE_FREE | XA_OWNERSHIP_EV_CAPABILITY |
                                        XA_OWNERSHIP_EV_CFG_CONSISTENT;
    for (int i = 0; i < symbol_count; i++) {
        const XaSymbol *symbol = symbols[i];
        if (!symbol)
            continue;
        const XaSymbolLinks *links = &symbol->links;
        bool has_move = links->ownership_candidate.id != 0 || links->final_move.id != 0;
        if (!has_move)
            continue;
        bool valid =
            links->ownership_candidate.complete && links->final_move.complete &&
            links->allocation_plan.complete && links->ownership_candidate.root != 0 &&
            links->ownership_candidate.source_symbol_id == symbol->id &&
            links->final_move.candidate_id == links->ownership_candidate.id &&
            links->final_move.storage_plan_id == links->allocation_plan.id &&
            (links->ownership_candidate.evidence & required_candidate) == required_candidate &&
            (links->final_move.evidence & (required_candidate | XA_OWNERSHIP_EV_STORAGE)) ==
                (required_candidate | XA_OWNERSHIP_EV_STORAGE);
        if (!valid) {
            if (source_line)
                *source_line = links->final_move.consume_line;
            xr_free(symbols);
            return OWNERSHIP_PUBLICATION_INVALID;
        }
    }
    xr_free(symbols);

    for (int i = 0; i < scope->child_count; i++) {
        OwnershipPublicationStatus status = verify_scope_ownership(scope->children[i], source_line);
        if (status != OWNERSHIP_PUBLICATION_OK)
            return status;
    }
    return OWNERSHIP_PUBLICATION_OK;
}

XaTypedProgramPublishResult xa_typed_program_publish(struct XaAnalyzer *analyzer,
                                                     const struct AstNode *syntax,
                                                     const struct XgGlobalEvidence *global_evidence,
                                                     uint32_t module_id) {
    XaTypedProgramPublishResult result = {0};
    if (!analyzer || !syntax || analyzer->semantic_revision == 0) {
        result.reason = XA_TYPED_PROGRAM_REASON_INVALID_INPUT;
        result.detail = "typed program requires analyzed syntax and a live semantic database";
        return result;
    }

    XaDiagnostic *diag = first_error(analyzer);
    if (diag) {
        result.reason = XA_TYPED_PROGRAM_REASON_ANALYZER_ERROR;
        result.source_line = diag->location.line > 0 ? (uint32_t) diag->location.line : 0;
        result.detail = diag->message ? diag->message : "semantic analysis failed";
        return result;
    }
    if (analyzer->recovery_poison_type_count != 0) {
        result.reason = XA_TYPED_PROGRAM_REASON_RECOVERY_POISON;
        result.detail = "semantic analysis contains recovery-poisoned executable types";
        return result;
    }

    OwnershipPublicationStatus ownership_status =
        verify_scope_ownership(analyzer->global_scope, &result.source_line);
    if (ownership_status != OWNERSHIP_PUBLICATION_OK) {
        result.reason = ownership_status == OWNERSHIP_PUBLICATION_RESOURCE_FAILURE
                            ? XA_TYPED_PROGRAM_REASON_ANALYSIS_RESOURCE_FAILURE
                            : XA_TYPED_PROGRAM_REASON_OWNERSHIP_PROOF;
        result.detail = ownership_status == OWNERSHIP_PUBLICATION_RESOURCE_FAILURE
                            ? "ownership publication failed (AnalysisResourceFailure)"
                            : "move ownership evidence is incomplete or inconsistent";
        return result;
    }

    XaTypedProgram *program = (XaTypedProgram *) xr_calloc(1, sizeof(XaTypedProgram));
    if (!program) {
        result.reason = XA_TYPED_PROGRAM_REASON_ANALYSIS_RESOURCE_FAILURE;
        result.detail = "typed program allocation failed (AnalysisResourceFailure)";
        return result;
    }
    program->syntax = syntax;
    program->semantics = analyzer;
    program->source_file = analyzer->current_file;
    program->global_evidence = global_evidence;
    program->semantic_revision = analyzer->semantic_revision;
    program->module_id = module_id;
    const XrModuleSpec *module_spec = find_module_spec(analyzer, syntax);
    program->source_module_authority_present =
        module_spec && module_spec->ast == syntax && module_spec->canonical &&
        xr_source_semantic_module_authority(module_spec->canonical,
                                            module_spec->source_content_fingerprint,
                                            &program->source_module_authority, NULL);
    if (!xa_node_table_snapshot_conversions((const XaNodeTable *) analyzer->node_table,
                                            &program->conversions, &program->conversion_count)) {
        xr_free(program);
        result.reason = XA_TYPED_PROGRAM_REASON_ANALYSIS_RESOURCE_FAILURE;
        result.detail = "conversion snapshot allocation failed (AnalysisResourceFailure)";
        return result;
    }
    XaScalarProgramAuthorityStatus scalar_status = xa_scalar_program_authority_publish(
        analyzer, syntax, module_spec, &program->scalar_authority);
    if (scalar_status == XA_SCALAR_PROGRAM_AUTHORITY_INVALID) {
        xr_free(program->conversions);
        xr_free(program);
        result.reason = XA_TYPED_PROGRAM_REASON_SCALAR_AUTHORITY;
        result.detail = "matched scalar program has incomplete or inconsistent authority";
        return result;
    }
    if (scalar_status == XA_SCALAR_PROGRAM_AUTHORITY_RESOURCE_FAILURE) {
        xr_free(program->conversions);
        xr_free(program);
        result.reason = XA_TYPED_PROGRAM_REASON_ANALYSIS_RESOURCE_FAILURE;
        result.detail = "scalar authority allocation failed (AnalysisResourceFailure)";
        return result;
    }
    if (!program->scalar_authority) {
        char closure_error[256] = {0};
        XaProgramSemanticClosurePublishStatus closure_status = xa_i64_overflow_program_publish(
            analyzer, syntax, module_spec, &program->program_semantic_closure, closure_error,
            sizeof(closure_error));
        if (closure_status == XA_PROGRAM_SEMANTIC_CLOSURE_UNSUPPORTED)
            closure_status = xa_program_semantic_closure_publish_leaf_aggregate(
                analyzer, syntax, module_spec, &program->program_semantic_closure, closure_error,
                sizeof(closure_error));
        if (closure_status == XA_PROGRAM_SEMANTIC_CLOSURE_INVALID ||
            closure_status == XA_PROGRAM_SEMANTIC_CLOSURE_RESOURCE_FAILURE) {
            xr_free(program->conversions);
            xr_free(program);
            result.reason = closure_status == XA_PROGRAM_SEMANTIC_CLOSURE_RESOURCE_FAILURE
                                ? XA_TYPED_PROGRAM_REASON_ANALYSIS_RESOURCE_FAILURE
                                : XA_TYPED_PROGRAM_REASON_PROGRAM_SEMANTIC_CLOSURE;
            result.detail =
                closure_status == XA_PROGRAM_SEMANTIC_CLOSURE_RESOURCE_FAILURE
                    ? "program closure allocation failed (AnalysisResourceFailure)"
                    : "matched program closure has incomplete or inconsistent authority";
            return result;
        }
    }
    program->verified = true;
    result.program = program;
    result.reason = XA_TYPED_PROGRAM_REASON_NONE;
    return result;
}

void xa_typed_program_free(XaTypedProgram *program) {
    if (!program)
        return;
    xa_scalar_program_authority_free(program->scalar_authority);
    xr_program_semantic_closure_free(program->program_semantic_closure);
    xr_free(program->conversions);
    xr_free(program);
}

bool xa_typed_program_is_verified(const XaTypedProgram *program) {
    return program && program->verified;
}

bool xa_typed_program_is_current(const XaTypedProgram *program) {
    return program && program->verified && program->semantics &&
           program->semantic_revision == program->semantics->semantic_revision;
}

uint64_t xa_typed_program_semantic_revision(const XaTypedProgram *program) {
    return program ? program->semantic_revision : 0;
}

const struct AstNode *xa_typed_program_syntax(const XaTypedProgram *program) {
    return program ? program->syntax : NULL;
}

struct XaAnalyzer *xa_typed_program_semantics(const XaTypedProgram *program) {
    return program ? program->semantics : NULL;
}

const char *xa_typed_program_source_file(const XaTypedProgram *program) {
    return program ? program->source_file : NULL;
}

const struct XgGlobalEvidence *xa_typed_program_global_evidence(const XaTypedProgram *program) {
    return program ? program->global_evidence : NULL;
}

uint32_t xa_typed_program_module_id(const XaTypedProgram *program) {
    return program ? program->module_id : 0;
}

const XaScalarProgramAuthority *xa_typed_program_scalar_authority(const XaTypedProgram *program) {
    return xa_typed_program_is_verified(program) ? program->scalar_authority : NULL;
}

const XrProgramSemanticModuleInput *
xa_typed_program_source_module_authority(const XaTypedProgram *program) {
    return xa_typed_program_is_verified(program) && program->source_module_authority_present
               ? &program->source_module_authority
               : NULL;
}

const XrProgramSemanticClosure *
xa_typed_program_program_semantic_closure(const XaTypedProgram *program) {
    return xa_typed_program_is_verified(program) ? program->program_semantic_closure : NULL;
}

const struct XaResolvedCall *xa_typed_program_resolved_call(const XaTypedProgram *program,
                                                            const struct AstNode *call_node) {
    if (!xa_typed_program_is_current(program) || !call_node)
        return NULL;
    return xa_analyzer_get_resolved_call(program->semantics, call_node);
}

bool xa_typed_program_conversion(const XaTypedProgram *program, const struct AstNode *node,
                                 XrConversionWitness *out_witness) {
    if (!xa_typed_program_is_current(program) || !node)
        return false;
    uint32_t low = 0;
    uint32_t high = program->conversion_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        const XaNodeConversionEntry *entry = &program->conversions[mid];
        if (entry->node_id < node->node_id) {
            low = mid + 1;
        } else if (entry->node_id > node->node_id) {
            high = mid;
        } else {
            if (out_witness)
                *out_witness = entry->witness;
            return true;
        }
    }
    return false;
}

const XaEffectSummary *xa_typed_program_effect_summary(const XaTypedProgram *program,
                                                       const XaSymbol *symbol) {
    if (!xa_typed_program_is_current(program) || !symbol ||
        symbol->links.effect_id == XA_EFFECT_NONE)
        return NULL;
    return xa_effect_db_get(program->semantics->effect_db, symbol->links.effect_id);
}

const XaMemoryEffectSummary *xa_typed_program_memory_effect_summary(const XaTypedProgram *program,
                                                                    const XaSymbol *symbol) {
    if (!xa_typed_program_is_current(program) || !symbol ||
        symbol->links.memory_effect_id == XA_MEMORY_EFFECT_NONE)
        return NULL;
    return xa_memory_effect_db_get(program->semantics->memory_effect_db,
                                   symbol->links.memory_effect_id);
}

const XaOwnershipCandidateProof *xa_typed_program_ownership_candidate(const XaTypedProgram *program,
                                                                      const XaSymbol *symbol) {
    if (!xa_typed_program_is_current(program) || !symbol ||
        !symbol->links.ownership_candidate.complete)
        return NULL;
    return &symbol->links.ownership_candidate;
}

const XaFinalMoveProof *xa_typed_program_final_move(const XaTypedProgram *program,
                                                    const XaSymbol *symbol) {
    if (!xa_typed_program_is_current(program) || !symbol || !symbol->links.final_move.complete)
        return NULL;
    return &symbol->links.final_move;
}

const XaAllocationInstancePlan *xa_typed_program_allocation_plan(const XaTypedProgram *program,
                                                                 const XaSymbol *symbol) {
    if (!xa_typed_program_is_current(program) || !symbol || !symbol->links.allocation_plan.complete)
        return NULL;
    return &symbol->links.allocation_plan;
}

const char *xa_typed_program_reason_name(XaTypedProgramReason reason) {
    switch (reason) {
        case XA_TYPED_PROGRAM_REASON_NONE:
            return "none";
        case XA_TYPED_PROGRAM_REASON_INVALID_INPUT:
            return "invalid_input";
        case XA_TYPED_PROGRAM_REASON_ANALYZER_ERROR:
            return "analyzer_error";
        case XA_TYPED_PROGRAM_REASON_RECOVERY_POISON:
            return "recovery_poison";
        case XA_TYPED_PROGRAM_REASON_STALE_REVISION:
            return "stale_revision";
        case XA_TYPED_PROGRAM_REASON_OWNERSHIP_PROOF:
            return "ownership_proof";
        case XA_TYPED_PROGRAM_REASON_SCALAR_AUTHORITY:
            return "scalar_authority";
        case XA_TYPED_PROGRAM_REASON_PROGRAM_SEMANTIC_CLOSURE:
            return "program_semantic_closure";
        case XA_TYPED_PROGRAM_REASON_ANALYSIS_RESOURCE_FAILURE:
            return "analysis_resource_failure";
    }
    return "invalid_reason";
}
