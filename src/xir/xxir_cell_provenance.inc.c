/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_cell_provenance.inc.c - One finite owner and scope proof for real Cell edges
 *
 * KEY CONCEPT:
 *   A Cell type grants no ownership. A rooted producer proves an owned capture;
 *   scoped parameters retain symbolic physical-parameter dependencies.
 */
#include "xxir_cell_provenance_internal.h"

typedef struct CellProofFunction {
    uint32_t begin, count, parameters, scope_begin, scopes, words;
    size_t dependency_begin;
} CellProofFunction;
struct XrXirCellProvenance {
    XrCompileResources *resources;
    uint32_t function_count, value_count, scope_count;
    CellProofFunction *functions;
    uint8_t *origins, *roles, *tracked;
    uint32_t *scope_parameters, *value_functions;
    uint64_t *dependencies;
};
typedef struct CellProofEdge {
    uint32_t from, to, next, reverse;
    bool identity;
} CellProofEdge;
typedef struct CellProofBuild {
    const XrXirCompileContext *context;
    const XrXirModule *module;
    XrXirCellProvenance *proof;
    XrXirStatus status;
    XrXirDiagnostic location;
    uint32_t *heads, *reverse_heads, *queue;
    uint8_t *queued, *uses;
    CellProofEdge *edges;
    uint32_t edge_count, edge_capacity, queue_read, queue_write, queue_count;
} CellProofBuild;

static bool cell_proof_work(CellProofBuild *build, uint64_t units) {
    if (build->status != XR_XIR_OK) return false;
    if (!xir_compile_work(build->context,units)) build->status = XR_XIR_BUDGET;
    return build->status == XR_XIR_OK;
}
static bool cell_proof_error(CellProofBuild *build, uint32_t function,
    uint32_t instruction, XrXirStatus status) {
    build->status = status;
    build->location = (XrXirDiagnostic){status,function,UINT32_MAX,instruction,XR_XIR_DIAGNOSTIC_NONE};
    return false;
}
static bool cell_proof_cell(const XrXirModule *module, XrXirType type) {
    return xr_xir_type_is_cell(module->types,type);
}
static uint32_t cell_proof_operand(const XrXirFunction *function,
    const XrXirInstruction *op, uint32_t ordinal) {
    return xr_xir_op_uses_operand_table(op->op) ?
        function->operands[op->args[0]+ordinal] : op->args[ordinal];
}
static uint64_t *cell_proof_dependencies(XrXirCellProvenance *proof, uint32_t node) {
    CellProofFunction *f = &proof->functions[proof->value_functions[node]];
    return f->words ? proof->dependencies+f->dependency_begin+(size_t)(node-f->begin)*f->words : NULL;
}
static bool cell_proof_enqueue(CellProofBuild *build, uint32_t node) {
    if (!cell_proof_work(build,1)) return false;
    if (build->queued[node]) return true;
    if (!cell_proof_work(build,5)) return false;
    build->queued[node] = 1;
    build->queue[build->queue_write] = node;
    if (++build->queue_write == build->proof->value_count) build->queue_write = 0;
    ++build->queue_count;
    return true;
}
static bool cell_proof_seed(CellProofBuild *build, uint32_t node, uint8_t mask) {
    if (!cell_proof_work(build,2)) return false;
    uint8_t next = build->proof->origins[node] | mask;
    if (next == build->proof->origins[node]) return true;
    if (!cell_proof_work(build,1)) return false;
    build->proof->origins[node] = next;
    return cell_proof_enqueue(build,node);
}
static bool cell_proof_edge(CellProofBuild *build, uint32_t from, uint32_t to, bool identity) {
    if (build->edge_count == build->edge_capacity)
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    if (!cell_proof_work(build,9)) return false;
    uint32_t id = build->edge_count++;
    build->edges[id] = (CellProofEdge){from,to,build->heads[from],build->reverse_heads[to],identity};
    build->heads[from] = id; build->reverse_heads[to] = id;
    return true;
}
static bool cell_proof_inventory(CellProofBuild *build) {
    const XrXirModule *module = build->module;
    uint64_t values = 0, edges = 0;
    if (!cell_proof_work(build,module->function_count)) return false;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        values += (uint64_t)function->parameter_count+function->instruction_count;
        /* Two aliases per local writer, plus expanded actual operand edges. */
        edges += (uint64_t)function->instruction_count*2+function->operand_count;
        if (values > UINT32_MAX || edges > UINT32_MAX)
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BUDGET);
    }
    XrXirCellProvenance *proof = xir_compile_calloc(build->context,1,sizeof(*proof),&build->status);
    if (!proof) return false;
    build->proof = proof; proof->resources = build->context->resources;
    proof->function_count = module->function_count; proof->value_count = (uint32_t)values;
    build->edge_capacity = (uint32_t)edges;
    proof->functions = xir_compile_calloc(build->context,module->function_count,sizeof(*proof->functions),&build->status);
    proof->origins = xir_compile_calloc(build->context,(size_t)values,1,&build->status);
    proof->roles = xir_compile_calloc(build->context,(size_t)values,1,&build->status);
    proof->tracked = xir_compile_calloc(build->context,(size_t)values,1,&build->status);
    proof->value_functions = xir_compile_calloc(build->context,(size_t)values,sizeof(uint32_t),&build->status);
    build->heads = xir_compile_alloc(build->context,(size_t)values*sizeof(uint32_t),&build->status);
    build->reverse_heads = xir_compile_alloc(build->context,(size_t)values*sizeof(uint32_t),&build->status);
    build->queue = xir_compile_alloc(build->context,(size_t)values*sizeof(uint32_t),&build->status);
    build->queued = xir_compile_calloc(build->context,(size_t)values,1,&build->status);
    build->uses = xir_compile_calloc(build->context,(size_t)values,1,&build->status);
    /* Every linked edge is completely initialized by its append operation;
     * unused capacity is never read. Keep the full finite allocation bound. */
    if (edges>SIZE_MAX/sizeof(*build->edges))
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BUDGET);
    build->edges = xir_compile_alloc(build->context,(size_t)edges*sizeof(*build->edges),&build->status);
    if (build->status != XR_XIR_OK || !cell_proof_work(build,values*4+module->function_count)) return false;
    uint32_t begin = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        uint32_t count = function->parameter_count+function->instruction_count;
        proof->functions[f] = (CellProofFunction){begin,count,function->parameter_count,0,0,0,0};
        for (uint32_t v = 0; v < count; ++v) {
            proof->value_functions[begin+v] = f;
            proof->tracked[begin+v] = cell_proof_cell(module,xr_xir_operand_type(function,v)) ? 1 : 0;
            build->heads[begin+v] = build->reverse_heads[begin+v] = UINT32_MAX;
        }
        begin += count;
    }
    return true;
}

/* Only actual references assign a capture or lexical-cleanup role. */
static bool cell_proof_roles(CellProofBuild *build) {
    XrXirCellProvenance *proof = build->proof;
    const XrXirModule *module = build->module;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            if (!cell_proof_work(build,1)) return false;
            const XrXirInstruction *op = &function->instructions[i];
            if (op->op != XR_XIR_FUNCTION_REF && op->op != XR_XIR_CALL &&
                op->op != XR_XIR_INVOKE && op->op != XR_XIR_CLEANUP_REGISTER) continue;
            uint32_t target = (uint32_t)op->immediate;
            if (target >= module->function_count) return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
            CellProofFunction *callee = &proof->functions[target];
            if (op->args[1] > callee->parameters) return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
            if (op->op == XR_XIR_CLEANUP_REGISTER && (!module->declarations ||
                module->declarations->functions[target].cleanup_owner != f+1))
                return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
            if (!cell_proof_work(build,op->args[1]*2u)) return false;
            uint8_t use = op->op == XR_XIR_FUNCTION_REF ? 1u : op->op == XR_XIR_CLEANUP_REGISTER ? 4u : 2u;
            for (uint32_t p = 0; p < op->args[1]; ++p)
                if (proof->tracked[callee->begin+p] == 1) build->uses[callee->begin+p] |= use;
            if (op->op == XR_XIR_FUNCTION_REF) {
                const XrXirTypeNode *signature = xr_xir_callable_signature(module->types, op->type);
                if (!signature || signature->parameter_count != callee->parameters-op->args[1])
                    return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
                if (!cell_proof_work(build,signature->parameter_count)) return false;
                for (uint32_t p = 0; p < signature->parameter_count; ++p) {
                    const XrXirCallableParameter *parameter = &signature->parameters[p];
                    if (!xr_xir_callable_parameter_storage_valid(module->types, parameter))
                        return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
                    if (parameter->mode != XR_PARAM_REF) continue;
                    if (op->args[1] || proof->tracked[callee->begin+p] != 1)
                        return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
                    /* A real unbound reference authorizes only the scoped call
                     * protocol. No owner origin follows from its signature. */
                    build->uses[callee->begin+p] |= 2u;
                }
            }
        }
    }
    size_t words = 0; uint64_t scopes = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        CellProofFunction *function = &proof->functions[f];
        function->scope_begin = (uint32_t)scopes;
        if (!cell_proof_work(build,(uint64_t)function->parameters*4+3)) return false;
        for (uint32_t p = 0; p < function->parameters; ++p) {
            uint32_t id = function->begin+p;
            if (proof->tracked[id] != 1) continue;
            uint8_t use = build->uses[id];
            if ((use & 1u) && (use & 6u)) return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_TYPE);
            if ((use & 4u) && (use & 3u)) return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_TYPE);
            proof->roles[id] = (use & 1u) ? XR_XIR_CELL_PROOF_OWNED_CAPTURE :
                (use & 4u) ? XR_XIR_CELL_PROOF_LEXICAL_CLEANUP :
                (use & 2u) ? XR_XIR_CELL_PROOF_SCOPED_REF : XR_XIR_CELL_PROOF_UNKNOWN;
            if (!(use & 1u)) ++function->scopes;
        }
        scopes += function->scopes;
        function->words = function->scopes/64u+(function->scopes%64u != 0);
        function->dependency_begin = words;
        if (function->words && (size_t)function->count > (SIZE_MAX-words)/function->words)
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BUDGET);
        words += (size_t)function->count*function->words;
    }
    if (words > SIZE_MAX/sizeof(uint64_t) || scopes > UINT32_MAX)
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BUDGET);
    proof->scope_count = (uint32_t)scopes;
    proof->scope_parameters = xir_compile_calloc(build->context,(size_t)scopes,sizeof(uint32_t),&build->status);
    proof->dependencies = xir_compile_calloc(build->context,words,sizeof(uint64_t),&build->status);
    if (build->status != XR_XIR_OK) return false;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        CellProofFunction *function = &proof->functions[f]; uint32_t compact = 0;
        for (uint32_t p = 0; p < function->parameters; ++p) {
            if (!cell_proof_work(build,1)) return false;
            uint32_t id = function->begin+p;
            if (proof->tracked[id] != 1 || proof->roles[id] == XR_XIR_CELL_PROOF_OWNED_CAPTURE) continue;
            if (!cell_proof_work(build,2)) return false;
            proof->scope_parameters[function->scope_begin+compact] = p;
            cell_proof_dependencies(proof,id)[compact/64u] = UINT64_C(1)<<(compact%64u);
            ++compact;
            if (!cell_proof_seed(build,id,XR_XIR_CELL_ORIGIN_SCOPED)) return false;
        }
    }
    return true;
}

static bool cell_proof_identity_producer(XrXirOp op) {
    return op == XR_XIR_COPY || op == XR_XIR_OWNED_RETAIN || op == XR_XIR_PHI ||
        op == XR_XIR_LOCAL_NEW || op == XR_XIR_OWNED_LOCAL_NEW ||
        op == XR_XIR_LOCAL_READ || op == XR_XIR_OWNED_LOCAL_READ;
}
static bool cell_proof_build_edges(CellProofBuild *build) {
    XrXirCellProvenance *proof = build->proof;
    const XrXirModule *module = build->module;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const XrXirFunction *function = &module->functions[f];
        uint32_t begin = proof->functions[f].begin;
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            if (!cell_proof_work(build,2)) return false;
            const XrXirInstruction *op = &function->instructions[i];
            uint32_t result = begin+function->parameter_count+i;
            bool cell = proof->tracked[result] == 1;
            if (cell && op->op == XR_XIR_CELL_NEW) {
                if (!cell_proof_seed(build,result,XR_XIR_CELL_ORIGIN_OWNED)) return false;
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
                if (!cell_proof_edge(build,begin+op->args[0],result,false)) return false;
            }
            uint32_t count = operand_count(function,op,module);
            if (!cell_proof_work(build,count)) return false;
            for (uint32_t a = 0; a < count; ++a) {
                if (op->op == XR_XIR_PHI && !(a & 1u)) continue;
                uint32_t source = begin+cell_proof_operand(function,op,a);
                if (cell && cell_proof_identity_producer(op->op)) {
                    if (!cell_proof_edge(build,source,result,true)) return false;
                } else if (op->op == XR_XIR_FUNCTION_REF) {
                    uint32_t target = (uint32_t)op->immediate;
                    uint32_t formal = proof->functions[target].begin+a;
                    if (proof->tracked[formal] == 1 && !cell_proof_edge(build,source,formal,true)) return false;
                }
                if ((op->op == XR_XIR_LOCAL_WRITE || op->op == XR_XIR_OWNED_LOCAL_WRITE) && a == 1) {
                    uint32_t destination = begin+op->args[0];
                    if (proof->tracked[destination] == 1 && !cell_proof_edge(build,source,destination,true)) return false;
                }
                if ((op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_GROUP_INIT) &&
                    proof->tracked[source] == 1 && !cell_proof_seed(build,source,XR_XIR_CELL_ORIGIN_MODULE)) return false;
            }
            if (op->op == XR_XIR_CELL_LOCAL_WRITE) {
                uint32_t local = op->args[0];
                if (local >= function->parameter_count &&
                    function->instructions[local-function->parameter_count].op == XR_XIR_LOCAL_UNINIT &&
                    proof->tracked[begin+local] == 1 &&
                    !cell_proof_seed(build,begin+local,XR_XIR_CELL_ORIGIN_OWNED)) return false;
            }
        }
    }
    return true;
}

static bool cell_proof_join(CellProofBuild *build, uint32_t from, uint32_t to) {
    XrXirCellProvenance *proof = build->proof;
    if (!cell_proof_work(build,3)) return false;
    uint8_t next = proof->origins[from] | proof->origins[to];
    bool changed = next != proof->origins[to];
    if (changed) { if (!cell_proof_work(build,1)) return false; proof->origins[to] = next; }
    if (proof->value_functions[from] == proof->value_functions[to]) {
        uint32_t words = proof->functions[proof->value_functions[from]].words;
        uint64_t *source = cell_proof_dependencies(proof,from), *destination = cell_proof_dependencies(proof,to);
        for (uint32_t w = 0; w < words; ++w) {
            if (!cell_proof_work(build,3)) return false;
            uint64_t joined = source[w] | destination[w];
            if (joined != destination[w]) {
                if (!cell_proof_work(build,1)) return false;
                destination[w] = joined; changed = true;
            }
        }
    }
    return !changed || cell_proof_enqueue(build,to);
}
static bool cell_proof_solve(CellProofBuild *build) {
    while (build->queue_count) {
        if (!cell_proof_work(build,5)) return false;
        uint32_t node = build->queue[build->queue_read];
        if (++build->queue_read == build->proof->value_count) build->queue_read = 0;
        --build->queue_count; build->queued[node] = 0;
        for (uint32_t e = build->heads[node]; e != UINT32_MAX; e = build->edges[e].next) {
            if (!cell_proof_work(build,2) || !cell_proof_join(build,node,build->edges[e].to)) return false;
        }
        /* Publication belongs to the identity, including aliases made earlier. */
        if (build->proof->origins[node] & XR_XIR_CELL_ORIGIN_MODULE)
            for (uint32_t e = build->reverse_heads[node]; e != UINT32_MAX; e = build->edges[e].reverse) {
                if (!cell_proof_work(build,3)) return false;
                if (build->edges[e].identity &&
                    !cell_proof_seed(build,build->edges[e].from,XR_XIR_CELL_ORIGIN_MODULE)) return false;
            }
    }
    return true;
}

static bool cell_proof_sink(CellProofBuild *build, uint32_t f, uint32_t i,
    uint32_t ordinal, uint32_t source) {
    const XrXirInstruction *op = &build->module->functions[f].instructions[i];
    XrXirCellProvenance *proof = build->proof;
    uint8_t origin = proof->origins[source];
    if (!origin || (origin & XR_XIR_CELL_ORIGIN_UNKNOWN)) return cell_proof_error(build,f,i,XR_XIR_BAD_VALUE);
    if (cell_proof_identity_producer(op->op)) return true;
    if ((op->op == XR_XIR_CELL_READ || op->op == XR_XIR_CELL_WRITE ||
        op->op == XR_XIR_CELL_PLACE || op->op == XR_XIR_CELL_LOCAL_WRITE) && !ordinal) return true;
    if (op->op == XR_XIR_LOCAL_WRITE || op->op == XR_XIR_OWNED_LOCAL_WRITE) return true;
    if (op->op == XR_XIR_CALL || op->op == XR_XIR_INVOKE ||
        op->op == XR_XIR_FUNCTION_REF || op->op == XR_XIR_CLEANUP_REGISTER) {
        uint32_t target = (uint32_t)op->immediate;
        uint8_t role = proof->roles[proof->functions[target].begin+ordinal];
        if (op->op == XR_XIR_FUNCTION_REF)
            return (role == XR_XIR_CELL_PROOF_OWNED_CAPTURE && origin == XR_XIR_CELL_ORIGIN_OWNED) ||
                cell_proof_error(build,f,i,XR_XIR_BAD_VALUE);
        if (op->op == XR_XIR_CLEANUP_REGISTER)
            return role == XR_XIR_CELL_PROOF_LEXICAL_CLEANUP || cell_proof_error(build,f,i,XR_XIR_BAD_VALUE);
        return role == XR_XIR_CELL_PROOF_SCOPED_REF || cell_proof_error(build,f,i,XR_XIR_BAD_VALUE);
    }
    if (op->op == XR_XIR_CALL_INDIRECT || op->op == XR_XIR_INVOKE_INDIRECT) {
        const XrXirFunction *function = &build->module->functions[f];
        XrXirType callee = xr_xir_operand_type(function,(uint32_t)op->immediate);
        const XrXirTypeNode *signature = xr_xir_callable_signature(build->module->types,callee);
        if (!signature || ordinal >= signature->parameter_count)
            return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
        const XrXirCallableParameter *parameter = &signature->parameters[ordinal];
        return (xr_xir_callable_parameter_storage_valid(build->module->types,parameter) &&
            parameter->mode == XR_PARAM_REF && parameter->type ==
                xr_xir_operand_type(function,source-proof->functions[f].begin)) ||
            cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
    }
    if (op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_GROUP_INIT)
        return origin == (XR_XIR_CELL_ORIGIN_OWNED | XR_XIR_CELL_ORIGIN_MODULE) ||
            cell_proof_error(build,f,i,XR_XIR_BAD_VALUE);
    /* Return, aggregate, capture, GO and unsupported retention fail closed. */
    return cell_proof_error(build,f,i,XR_XIR_BAD_VALUE);
}
static bool cell_proof_validate(CellProofBuild *build) {
    XrXirCellProvenance *proof = build->proof;
    for (uint32_t f = 0; f < build->module->function_count; ++f) {
        const XrXirFunction *function = &build->module->functions[f];
        uint32_t begin = proof->functions[f].begin;
        for (uint32_t p = 0; p < function->parameter_count; ++p) {
            if (!cell_proof_work(build,2)) return false;
            if (proof->roles[begin+p] == XR_XIR_CELL_PROOF_OWNED_CAPTURE &&
                proof->origins[begin+p] != XR_XIR_CELL_ORIGIN_OWNED)
                return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_VALUE);
        }
        for (uint32_t i = 0; i < function->instruction_count; ++i) {
            if (!cell_proof_work(build,1)) return false;
            const XrXirInstruction *op = &function->instructions[i];
            uint32_t count = operand_count(function,op,build->module);
            for (uint32_t a = 0; a < count; ++a) {
                if (!cell_proof_work(build,2)) return false;
                if (op->op == XR_XIR_PHI && !(a & 1u)) continue;
                uint32_t source = begin+cell_proof_operand(function,op,a);
                if (proof->tracked[source] == 1 && !cell_proof_sink(build,f,i,a,source)) return false;
            }
        }
    }
    return true;
}

XR_FUNC void xr_xir_compile_cell_provenance_free(XrXirCellProvenance *proof) {
    if (!proof) return;
    xr_compile_resources_free(proof->functions); xr_compile_resources_free(proof->origins);
    xr_compile_resources_free(proof->roles); xr_compile_resources_free(proof->tracked);
    xr_compile_resources_free(proof->scope_parameters); xr_compile_resources_free(proof->value_functions);
    xr_compile_resources_free(proof->dependencies); xr_compile_resources_free(proof);
}
XR_FUNC XrXirStatus xr_xir_compile_cell_provenance_verified(const XrXirCompileContext *context,
    const XrXirModule *module, XrXirCellProvenance **output, XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module || !output || *output ||
        (module->function_count && !module->functions)) return XR_XIR_BAD_STRUCTURE;
    CellProofBuild build = {0}; build.context = context; build.module = module;
    build.location = (XrXirDiagnostic){XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    if (cell_proof_inventory(&build) && cell_proof_roles(&build) &&
        cell_proof_build_edges(&build) && cell_proof_solve(&build)) {
        /* Bottom is not ownership. Unrooted cycles taint every consumer before admission. */
        for (uint32_t v = 0; v < build.proof->value_count && build.status == XR_XIR_OK; ++v) {
            if (!cell_proof_work(&build,2)) break;
            if (build.proof->tracked[v] == 1 && !build.proof->origins[v])
                cell_proof_seed(&build,v,XR_XIR_CELL_ORIGIN_UNKNOWN);
        }
        if (build.status == XR_XIR_OK && cell_proof_solve(&build)) cell_proof_validate(&build);
    }
    xr_compile_resources_free(build.heads); xr_compile_resources_free(build.reverse_heads);
    xr_compile_resources_free(build.queue); xr_compile_resources_free(build.queued);
    xr_compile_resources_free(build.uses); xr_compile_resources_free(build.edges);
    build.location.status = build.status;
    if (diagnostic) *diagnostic = build.location;
    if (build.status != XR_XIR_OK) xr_xir_compile_cell_provenance_free(build.proof);
    else *output = build.proof;
    return build.status;
}
XR_FUNC uint32_t xr_xir_cell_provenance_origin(const XrXirCellProvenance *proof,
    uint32_t function, uint32_t value) {
    if (!proof || function >= proof->function_count || value >= proof->functions[function].count)
        return XR_XIR_CELL_ORIGIN_UNKNOWN;
    return proof->origins[proof->functions[function].begin+value];
}
XR_FUNC XrXirCellProofRole xr_xir_cell_provenance_role(const XrXirCellProvenance *proof,
    uint32_t function, uint32_t parameter) {
    if (!proof || function >= proof->function_count || parameter >= proof->functions[function].parameters)
        return XR_XIR_CELL_PROOF_UNKNOWN;
    return (XrXirCellProofRole)proof->roles[proof->functions[function].begin+parameter];
}
XR_FUNC XrXirStatus xr_xir_compile_cell_origin_view(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof, uint32_t function, uint32_t value, XrXirCellOriginView *output) {
    if (!xir_compile_context_valid(context) || !proof || proof->resources != context->resources ||
        !output || function >= proof->function_count ||
        value >= proof->functions[function].count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,7)) return XR_XIR_BUDGET;
    const CellProofFunction *f = &proof->functions[function]; uint32_t node = f->begin+value;
    uint8_t origin = proof->origins[node];
    uint32_t intrinsic = (origin & XR_XIR_CELL_ORIGIN_MODULE ? XR_XIR_CELL_ACCESS_ROOT : 0) |
        (origin & XR_XIR_CELL_ORIGIN_UNKNOWN ? XR_XIR_CELL_ACCESS_UNKNOWN : 0);
    *output = (XrXirCellOriginView){intrinsic,
        f->words ? proof->dependencies+f->dependency_begin+(size_t)value*f->words : NULL,
        f->words,f->scopes ? proof->scope_parameters+f->scope_begin : NULL,f->scopes};
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_cell_access(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof, const XrXirCellAccessRequest *request, uint32_t *output) {
    if (!request || !output || !proof || request->function >= proof->function_count ||
        (request->parameter_owners ? request->parameter_count != proof->functions[request->function].parameters :
            request->parameter_count != 0)) return XR_XIR_BAD_STRUCTURE;
    XrXirCellOriginView view = {0};
    XrXirStatus status = xr_xir_compile_cell_origin_view(context,proof,request->function,request->value,&view);
    if (status != XR_XIR_OK) return status;
    uint32_t result = view.intrinsic_mask;
    for (uint32_t b = 0; b < view.parameter_count; ++b) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (!(view.dependencies[b/64u] & (UINT64_C(1)<<(b%64u)))) continue;
        if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
        uint8_t owner = request->parameter_owners ? request->parameter_owners[view.parameters[b]] : XR_XIR_CELL_ACCESS_UNKNOWN;
        if (owner & ~(XR_XIR_CELL_ACCESS_ROOT | XR_XIR_CELL_ACCESS_UNKNOWN)) return XR_XIR_BAD_STRUCTURE;
        result |= owner;
    }
    *output = result;
    return XR_XIR_OK;
}
XR_FUNC XrXirStatus xr_xir_compile_cell_roles_verified(const XrXirCompileContext *context,
    const XrXirModule *module, uint32_t *offsets, uint8_t *roles, XrXirDiagnostic *diagnostic) {
    if (!offsets) return XR_XIR_BAD_STRUCTURE;
    XrXirCellProvenance *proof = NULL;
    XrXirStatus status = xr_xir_compile_cell_provenance_verified(context,module,&proof,diagnostic);
    if (status != XR_XIR_OK) return status;
    uint64_t total = 0;
    if (!xir_compile_work(context,(uint64_t)proof->function_count*2)) status = XR_XIR_BUDGET;
    if (status == XR_XIR_OK)
        for (uint32_t f = 0; f < proof->function_count; ++f) total += proof->functions[f].parameters;
    if (status == XR_XIR_OK && ((total && !roles) || total > UINT32_MAX)) status = XR_XIR_BAD_STRUCTURE;
    if (status == XR_XIR_OK && !xir_compile_work(context,(uint64_t)proof->function_count*2+1+total*2)) status = XR_XIR_BUDGET;
    if (status == XR_XIR_OK) {
        uint32_t cursor = 0;
        for (uint32_t f = 0; f < proof->function_count; ++f) {
            offsets[f] = cursor;
            const CellProofFunction *function = &proof->functions[f];
            for (uint32_t p = 0; p < function->parameters; ++p) roles[cursor++] = proof->roles[function->begin+p];
        }
        offsets[proof->function_count] = cursor;
    }
    xr_xir_compile_cell_provenance_free(proof);
    if (diagnostic) diagnostic->status = status;
    return status;
}
