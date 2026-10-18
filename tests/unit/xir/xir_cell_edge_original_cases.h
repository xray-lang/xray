/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_cell_edge_original_cases.h - Independent original two-pass proof replay
 */
#ifndef XIR_CELL_EDGE_ORIGINAL_CASES_H
#define XIR_CELL_EDGE_ORIGINAL_CASES_H

/* Independent original count/write graph and complete proof replay. */
static bool edge_original_counting;
static bool edge_original_edge(CellProofBuild *build, uint32_t from, uint32_t to, bool identity) {
    if (!cell_proof_work(build,3)) return false;
    if (from >= build->proof->value_count || to >= build->proof->value_count)
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    if (edge_original_counting) {
        if (build->edge_count == UINT32_MAX)
            return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BUDGET);
        if (!cell_proof_work(build,1)) return false;
        ++build->edge_count; return true;
    }
    if (build->edge_count == build->edge_capacity)
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    if (!cell_proof_work(build,9)) return false;
    uint32_t id = build->edge_count++;
    build->edges[id] = (CellProofEdge){from,to,build->heads[from],build->reverse_heads[to],identity};
    build->heads[from] = id; build->reverse_heads[to] = id;
    return true;
}

static bool edge_original_edge_row(CellProofBuild *build,uint32_t f,uint32_t i,
    const XrXirCellProofBodyHead *function,const XrXirInstruction *op) {
    XrXirCellProvenance *proof=build->proof;const XirMetadataDomain *module=&build->body->domain;
    uint32_t begin=proof->functions[f].begin;
    uint32_t result = begin+function->parameter_count+i;
    bool cell = cell_proof_tracking(proof,f,function->parameter_count+i) == 1;
    if (op->op == XR_XIR_CELL_PROJECT && !cell_proof_project_identity(build,f,op->args[0])) return false;
    if (cell && op->op == XR_XIR_CELL_NEW) {
        if (!cell_proof_seed(build,result,XR_XIR_CELL_ORIGIN_OWNED)) return false;
    } else if (cell && op->op == XR_XIR_CELL_PROJECT) {
        if (!cell_proof_seed(build,result,XR_XIR_CELL_ORIGIN_SCOPED) ||
            !edge_original_edge(build,begin+op->args[0],result,false)) return false;
    } else if (cell && op->op == XR_XIR_SLOT_LOAD) {
        if (!module->declarations || op->immediate < 0 ||
            (uint64_t)op->immediate >= module->declarations->slot_count ||
            !module->declarations->slots[op->immediate].mutable)
            return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
        if (!cell_proof_seed(build,result,XR_XIR_CELL_ORIGIN_MODULE)) return false;
    } else if (cell && !cell_proof_identity_producer(op->op) && op->op != XR_XIR_LOCAL_UNINIT) {
        if (!cell_proof_seed(build,result,XR_XIR_CELL_ORIGIN_UNKNOWN)) return false;
    }
    if (op->op == XR_XIR_SLOT_PLACE) {
        proof->tracked[result] = 2;
        if (!cell_proof_seed(build,result,XR_XIR_CELL_ORIGIN_MODULE)) return false;
    } else if (op->op == XR_XIR_CELL_PLACE || op->op == XR_XIR_FIELD_PLACE ||
        op->op == XR_XIR_INDEX_PLACE || op->op == XR_XIR_OBJECT_PLACE) {
        /* Logical place values can carry an access dependency, never own a Cell. */
        proof->tracked[result] = 2;
        if (!edge_original_edge(build,begin+op->args[0],result,false)) return false;
    }
    uint32_t count = cell_proof_operand_count(build,f,function,op);
    if (!cell_proof_work(build,(uint64_t)count*3)) return false;
    if (xr_xir_op_uses_operand_table(op->op) &&
        (op->args[0] > function->operand_count || count > function->operand_count-op->args[0]))
        return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
    for (uint32_t a = 0; a < count; ++a) {
        if (op->op == XR_XIR_PHI && !(a & 1u)) continue;
        uint32_t value = cell_proof_operand(function,op,a);
        if (value >= proof->functions[f].count)
            return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
        uint32_t source = begin+value;
        if (cell && cell_proof_identity_producer(op->op)) {
            if (!edge_original_edge(build,source,result,true)) return false;
        } else if (op->op == XR_XIR_FUNCTION_REF) {
            uint32_t target = (uint32_t)op->immediate;
            uint32_t formal = proof->functions[target].begin+a;
            if (cell_proof_tracking(proof,target,a) == 1) {
                if (!proof->functions[f].stored)
                    return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
                if (!edge_original_edge(build,source,formal,true)) return false;
            }
        }
        if ((op->op == XR_XIR_LOCAL_WRITE || op->op == XR_XIR_OWNED_LOCAL_WRITE) && a == 1) {
            uint32_t destination = begin+op->args[0];
            if (op->args[0] >= proof->functions[f].count)
                return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
            if (cell_proof_tracking(proof,f,op->args[0]) == 1 &&
                !edge_original_edge(build,source,destination,true)) return false;
        }
        if ((op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_GROUP_INIT) &&
            cell_proof_tracking(proof,f,value) == 1 && !cell_proof_seed(build,source,XR_XIR_CELL_ORIGIN_MODULE)) return false;
    }
    if (op->op == XR_XIR_CELL_LOCAL_WRITE) {
        uint32_t local = op->args[0];
        if (local >= proof->functions[f].count)
            return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
        if (local >= function->parameter_count &&
            cell_proof_local_uninit(build,f,local-function->parameter_count) &&
            cell_proof_tracking(proof,f,local) == 1 &&
            !cell_proof_seed(build,begin+local,XR_XIR_CELL_ORIGIN_OWNED)) return false;
    }
    return true;
}

static bool edge_original_build_edges(CellProofBuild *build) {
    for (uint32_t f=0;f<build->body->domain.function_count;++f) {
        XrXirCellProofBodyHead head={0};if (!cell_proof_head(build,f,&head)) return false;
        if (!build->body->range) {
            for (uint32_t i=0;i<head.instruction_count;++i) {
                if (!cell_proof_work(build,3)) return false;
                XrXirInstruction operation={0};const XrXirInstruction *op=cell_proof_instruction(build,f,i,&operation);
                if (!op || !edge_original_edge_row(build,f,i,&head,op)) return false;
            }
            continue;
        }
        if (!cell_proof_work(build,sizeof(XrXirCellProofRunCursor))) return false;
        XrXirCellProofRunCursor cursor={.function=f};
        while (cursor.next<head.instruction_count) {
            XrXirCellProofRun run;bool handled=false;
            if (!cell_proof_run(build,&head,&cursor,&run) ||
                !cell_proof_edges_range(build,f,&head,&run,&handled)) return false;
            if (handled) continue;
            for (uint32_t i=run.first;i-run.first<run.count;++i) {
                if (!cell_proof_work(build,3)) return false;
                XrXirInstruction operation;const XrXirInstruction *op=
                    cell_proof_run_operation(build,&cursor,&run,i,&operation);
                if (!op || !edge_original_edge_row(build,f,i,&head,op)) return false;
            }
        }
    }
    return true;
}

static bool edge_original_prepare_edges(CellProofBuild *build) {
    edge_original_counting = true;
    bool counted = edge_original_build_edges(build);
    edge_original_counting = false;
    if (!counted) return false;
    build->edge_capacity = build->edge_count; build->edge_count = 0;
    size_t bytes = 0;
    if (!cell_proof_edge_storage_bytes(build->edge_capacity,&bytes))
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BUDGET);
    if (!cell_proof_work(build,4)) return false;
    build->edges = xir_compile_alloc(build->context,bytes,&build->status);
    if (build->status != XR_XIR_OK || !edge_original_build_edges(build)) return false;
    if (!cell_proof_work(build,1)) return false;
    return build->edge_count == build->edge_capacity ||
        cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
}

static void edge_original_complete(CellProofBuild *build) {
    /* Full inventory proves that no physical Cell or access place exists.
     * The verified caller still owns every call, signature and SSA check. */
    if (build->status==XR_XIR_OK && build->proof->value_count &&
        cell_proof_class_collect(build) && cell_proof_class_storage(build) && cell_proof_roles(build) &&
        edge_original_prepare_edges(build) && cell_proof_solve(build)) {
        /* Bottom is not ownership. Unrooted cycles taint every consumer before admission. */
        for (uint32_t v = 0; v < build->proof->value_count && build->status == XR_XIR_OK; ++v) {
            if (!cell_proof_work(build,2)) break;
            if (build->proof->tracked[v] == 1 && !build->proof->origins[v])
                cell_proof_seed(build,v,XR_XIR_CELL_ORIGIN_UNKNOWN);
        }
        if (build->status == XR_XIR_OK && cell_proof_solve(build)) cell_proof_validate(build);
    }
}

static XrXirStatus edge_original_derive(const XrXirCompileContext *context,
    const XrXirCellProofBodyRead *body, XrXirCellProvenance **output, XrXirDiagnostic *diagnostic) {
    if (!cell_proof_body_valid(context,body) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    CellProofBuild build = {0}; build.context = context; build.body = body;
    build.location = (XrXirDiagnostic){XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    if (cell_proof_inventory(&build)) edge_original_complete(&build);
    cell_proof_workspace_free(&build);
    build.location.status = build.status;
    if (diagnostic) *diagnostic = build.location;
    if (build.status != XR_XIR_OK) xr_xir_compile_cell_provenance_free(build.proof);
    else *output = build.proof;
    return build.status;
}

#endif // XIR_CELL_EDGE_ORIGINAL_CASES_H
