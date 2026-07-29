/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xa_typed_program.h"
#include "xa_node_table.h"
#include "xanalyzer.h"
#include "xanalyzer_symbol.h"
#include "../parser/xast_nodes.h"
#include "../../base/xmalloc.h"

struct XaTypedProgram {
    const struct AstNode *syntax;
    struct XaAnalyzer *semantics;
    const struct XgGlobalEvidence *global_evidence;
    uint64_t semantic_revision;
    uint32_t module_id;
    XaNodeConversionEntry *conversions;
    uint32_t conversion_count;
    bool verified;
};

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
    program->global_evidence = global_evidence;
    program->semantic_revision = analyzer->semantic_revision;
    program->module_id = module_id;
    if (!xa_node_table_snapshot_conversions((const XaNodeTable *) analyzer->node_table,
                                            &program->conversions,
                                            &program->conversion_count)) {
        xr_free(program);
        result.reason = XA_TYPED_PROGRAM_REASON_ANALYSIS_RESOURCE_FAILURE;
        result.detail = "conversion snapshot allocation failed (AnalysisResourceFailure)";
        return result;
    }
    program->verified = true;
    result.program = program;
    result.reason = XA_TYPED_PROGRAM_REASON_NONE;
    return result;
}

void xa_typed_program_free(XaTypedProgram *program) {
    if (!program)
        return;
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

const struct XgGlobalEvidence *xa_typed_program_global_evidence(const XaTypedProgram *program) {
    return program ? program->global_evidence : NULL;
}

uint32_t xa_typed_program_module_id(const XaTypedProgram *program) {
    return program ? program->module_id : 0;
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
        case XA_TYPED_PROGRAM_REASON_ANALYSIS_RESOURCE_FAILURE:
            return "analysis_resource_failure";
    }
    return "invalid_reason";
}
