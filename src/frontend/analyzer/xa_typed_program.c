/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 */

#include "xa_typed_program.h"
#include "xanalyzer.h"
#include "../../base/xmalloc.h"

struct XaTypedProgram {
    const struct AstNode *syntax;
    struct XaAnalyzer *semantics;
    const struct XgGlobalEvidence *global_evidence;
    uint64_t semantic_revision;
    uint32_t module_id;
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

    XaTypedProgram *program = (XaTypedProgram *) xr_calloc(1, sizeof(XaTypedProgram));
    if (!program) {
        result.reason = XA_TYPED_PROGRAM_REASON_INVALID_INPUT;
        result.detail = "failed to allocate typed program publication";
        return result;
    }
    program->syntax = syntax;
    program->semantics = analyzer;
    program->global_evidence = global_evidence;
    program->semantic_revision = analyzer->semantic_revision;
    program->module_id = module_id;
    program->verified = true;
    result.program = program;
    result.reason = XA_TYPED_PROGRAM_REASON_NONE;
    return result;
}

void xa_typed_program_free(XaTypedProgram *program) {
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
    }
    return "invalid_reason";
}
