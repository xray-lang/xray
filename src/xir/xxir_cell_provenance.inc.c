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
    uint32_t begin, count, parameters, scope_begin, scopes, words, stored;
    size_t dependency_begin;
    uint32_t class_begin,class_count,class_words;
    size_t class_dependency_begin;
} CellProofFunction;
struct XrXirCellProvenance {
    XrCompileResources *resources;
    uint32_t function_count, value_count, scope_count;
    CellProofFunction *functions;
    uint8_t *origins, *roles, *tracked;
    uint32_t *scope_parameters, *value_functions;
    uint64_t *dependencies;
    uint32_t class_count,project_count;
    size_t class_dependency_count;
    XrXirCellClassRoot *class_roots;
    uint64_t *class_dependencies;
};
typedef struct CellProofEdge {
    uint32_t from, to, next, reverse;
    bool identity;
} CellProofEdge;
typedef struct CellProofBuild {
    const XrXirCompileContext *context;
    const XrXirCellProofBodyRead *body;
    XrXirCellProvenance *proof;
    XrXirStatus status;
    XrXirDiagnostic location;
    uint32_t *heads, *reverse_heads, *queue;
    uint8_t *queued, *uses;
    CellProofEdge *edges;
    uint32_t edge_count, edge_capacity, queue_read, queue_write, queue_count;
    bool counting_edges;
} CellProofBuild;

static bool cell_proof_work(CellProofBuild *build, uint64_t units) {
    if (build->status != XR_XIR_OK) return false;
    if (!xir_compile_work(build->context,units)) build->status = XR_XIR_BUDGET;
    return build->status == XR_XIR_OK;
}
static bool cell_proof_error(CellProofBuild *build, uint32_t function,
    uint32_t instruction, XrXirStatus status) {
    if (build->status != XR_XIR_OK) return false;
    build->status = status;
    build->location = (XrXirDiagnostic){status,function,UINT32_MAX,instruction,XR_XIR_DIAGNOSTIC_NONE};
    return false;
}
#include "xxir_cell_proof_body_read.inc.c"

static bool cell_proof_cell(const XirMetadataDomain *domain,XrXirType type) {
    return xr_xir_type_is_cell(domain->types,type);
}
/* Every physical value is classified before any graph storage is omitted.
 * Place nodes carry access facts even when their logical type is not Cell. */
static bool cell_proof_place(XrXirOp op) {
    return op == XR_XIR_SLOT_PLACE || op == XR_XIR_CELL_PLACE ||
        op == XR_XIR_FIELD_PLACE || op == XR_XIR_INDEX_PLACE || op == XR_XIR_OBJECT_PLACE;
}
static uint8_t cell_proof_tracking(const XrXirCellProvenance *proof,
    uint32_t function, uint32_t value) {
    const CellProofFunction *f = &proof->functions[function];
    return f->stored ? proof->tracked[f->begin+value] : 0;
}
static uint32_t cell_proof_operand(const XrXirCellProofBodyHead *function,
    const XrXirInstruction *op, uint32_t ordinal) {
    return xr_xir_op_uses_operand_table(op->op) ?
        function->operands[op->args[0]+ordinal] : op->args[ordinal];
}
static uint64_t *cell_proof_dependencies(XrXirCellProvenance *proof, uint32_t node) {
    CellProofFunction *f = &proof->functions[proof->value_functions[node]];
    return f->words ? proof->dependencies+f->dependency_begin+(size_t)(node-f->begin)*f->words : NULL;
}
static uint64_t *cell_proof_class_dependencies(XrXirCellProvenance *proof,uint32_t node) {
    CellProofFunction *f=&proof->functions[proof->value_functions[node]];
    return f->class_words ? proof->class_dependencies+f->class_dependency_begin+
        (size_t)(node-f->begin)*f->class_words : NULL;
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
    if (!cell_proof_work(build,3)) return false;
    if (from >= build->proof->value_count || to >= build->proof->value_count)
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    if (build->counting_edges) {
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
/* The writer and callback walker submit the same physical row. This sink
 * owns no permission: it only accumulates genuine Cell and Place inventory,
 * and the ordinary role, edge, solve and validation stages still follow. */
static bool cell_proof_inventory_value(CellProofBuild *build,uint32_t f,
    const XrXirCellProofBodyHead *function,uint32_t value) {
    if (!function || value>=(uint64_t)function->parameter_count+function->instruction_count)
        return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    return true;
}
static bool cell_proof_inventory_parameter(CellProofBuild *build,uint32_t f,
    const XrXirCellProofBodyHead *function,uint32_t value,XrXirType type,bool *tracked) {
    if (!tracked) return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    if (!cell_proof_inventory_value(build,f,function,value)) return false;
    if (value>=function->parameter_count || !function->parameters || type!=function->parameters[value])
        return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_TYPE);
    *tracked|=cell_proof_cell(&build->body->domain,type);return true;
}
/* Both readers validate the physical range and independent actual type before
 * this shared instruction inventory; no shape temporary or permission is passed. */
static bool cell_proof_inventory_instruction(CellProofBuild *build,uint32_t f,
    uint32_t i,XrXirType type,XrXirOp opcode,bool *tracked) {
    if (!tracked) return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
    if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT)
        return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
    *tracked|=cell_proof_place(opcode);
    if (opcode==XR_XIR_CELL_PROJECT) {
        if (!cell_proof_work(build,2)) return false;
        if (build->proof->project_count==UINT32_MAX)
            return cell_proof_error(build,f,i,XR_XIR_BUDGET);
        ++build->proof->project_count;
    }
    *tracked|=cell_proof_cell(&build->body->domain,type);return true;
}

static bool cell_proof_inventory_begin(CellProofBuild *build) {
    const XirMetadataDomain *module = &build->body->domain;
    if (module->types && module->types->count && !module->types->nodes)
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    XrXirCellProvenance *proof = xir_compile_calloc(build->context,1,sizeof(*proof),&build->status);
    if (!proof) return false;
    build->proof = proof; proof->resources = build->context->resources;
    proof->function_count = module->function_count;
    proof->functions = xir_compile_calloc(build->context,module->function_count,sizeof(*proof->functions),&build->status);
    if (build->status != XR_XIR_OK) return false;
    return true;
}


static bool cell_proof_inventory_store(CellProofBuild *build,uint32_t f,uint32_t first,
    uint32_t count,XrXirType type) {
    const CellProofFunction *stored=&build->proof->functions[f];
    if (first>stored->stored || count>stored->stored-first)
        return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    if (!cell_proof_work(build,3)) return false;
    uint8_t tracked=cell_proof_cell(&build->body->domain,type)?1u:0u;
    for (uint32_t v=first;v-first<count;++v) {
        uint32_t id=stored->begin+v;
        build->proof->value_functions[id]=f;build->proof->tracked[id]=tracked;
        build->heads[id]=build->reverse_heads[id]=UINT32_MAX;
    }
    return true;
}

static bool cell_proof_inventory_storage(CellProofBuild *build,uint64_t values) {
    const XirMetadataDomain *module=&build->body->domain;
    XrXirCellProvenance *proof=build->proof;
    proof->value_count = (uint32_t)values;
    if (values > SIZE_MAX/sizeof(uint32_t))
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BUDGET);
    proof->origins = xir_compile_calloc(build->context,(size_t)values,1,&build->status);
    proof->roles = xir_compile_calloc(build->context,(size_t)values,1,&build->status);
    proof->tracked = xir_compile_calloc(build->context,(size_t)values,1,&build->status);
    proof->value_functions = xir_compile_calloc(build->context,(size_t)values,sizeof(uint32_t),&build->status);
    build->heads = xir_compile_alloc(build->context,(size_t)values*sizeof(uint32_t),&build->status);
    build->reverse_heads = xir_compile_alloc(build->context,(size_t)values*sizeof(uint32_t),&build->status);
    build->queue = xir_compile_alloc(build->context,(size_t)values*sizeof(uint32_t),&build->status);
    build->queued = xir_compile_calloc(build->context,(size_t)values,1,&build->status);
    build->uses = xir_compile_calloc(build->context,(size_t)values,1,&build->status);
    if (build->status != XR_XIR_OK || !cell_proof_work(build,values*4)) return false;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        const CellProofFunction *stored = &proof->functions[f];
        if (!build->body->range) {
            for (uint32_t v=0;v<stored->stored;++v) {
                uint32_t id=stored->begin+v;
                proof->value_functions[id]=f;
                proof->tracked[id]=cell_proof_cell(module,cell_proof_type(build,f,v))?1:0;
                build->heads[id]=build->reverse_heads[id]=UINT32_MAX;
            }
            continue;
        }
        if (!stored->stored) continue;
        XrXirCellProofBodyHead head={0};if (!cell_proof_head(build,f,&head)) return false;
        if ((uint64_t)head.parameter_count+head.instruction_count!=stored->stored ||
            head.parameter_count!=stored->parameters) return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
        for (uint32_t v=0;v<head.parameter_count;++v) {
            XrXirType type=cell_proof_type(build,f,v);
            if (build->status!=XR_XIR_OK || !cell_proof_inventory_store(build,f,v,1,type)) return false;
        }
        if (!cell_proof_work(build,sizeof(XrXirCellProofRunCursor))) return false;
        XrXirCellProofRunCursor cursor={.function=f};
        while (cursor.next<head.instruction_count) {
            XrXirCellProofRun run;if (!cell_proof_run(build,&head,&cursor,&run)) return false;
            if (!(run.varying&UINT32_C(2))) {
                if (!cell_proof_inventory_store(build,f,head.parameter_count+run.first,run.count,run.operation.type))
                    return false;
            } else for (uint32_t i=run.first;i-run.first<run.count;++i) {
                XrXirType type=cell_proof_type(build,f,head.parameter_count+i);
                if (build->status!=XR_XIR_OK ||
                    !cell_proof_inventory_store(build,f,head.parameter_count+i,1,type)) return false;
            }
        }
    }
    return true;
}



static bool cell_proof_inventory_run(CellProofBuild *build,uint32_t f,
    const XrXirCellProofBodyHead *head,const XrXirCellProofRun *run,bool *tracked) {
    if (!cell_proof_work(build,6)) return false;
    if (run->first>=head->instruction_count || run->count>head->instruction_count-run->first)
        return cell_proof_error(build,f,run->first,XR_XIR_BAD_STRUCTURE);
    if (build->body->range && !(run->varying&UINT32_C(3))) {
        *tracked|=cell_proof_place(run->operation.op) || cell_proof_cell(&build->body->domain,run->operation.type);
        if (run->operation.op==XR_XIR_CELL_PROJECT) {
            if (run->count>UINT32_MAX-build->proof->project_count)
                return cell_proof_error(build,f,run->first,XR_XIR_BUDGET);
            if (!cell_proof_work(build,sizeof(build->proof->project_count)+1)) return false;
            build->proof->project_count+=run->count;
        }
        return true;
    }
    for (uint32_t i=run->first;i-run->first<run->count;++i) {
        XrXirType type=cell_proof_type(build,f,head->parameter_count+i);
        if (build->status!=XR_XIR_OK || !cell_proof_work(build,3)) return false;
        XrXirInstruction operation;const XrXirInstruction *op=cell_proof_run_operation(build,f,run,i,&operation);
        if (!op) return false;
        if (op->type!=type) return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
        if (!cell_proof_inventory_instruction(build,f,i,type,op->op,tracked)) return false;
    }
    return true;
}

static bool cell_proof_inventory(CellProofBuild *build) {
    const XirMetadataDomain *module=&build->body->domain;
    if (!cell_proof_inventory_begin(build)) return false;
    XrXirCellProvenance *proof=build->proof;
    uint64_t values = 0, physical = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        XrXirCellProofBodyHead head={0};if (!cell_proof_head(build,f,&head)) return false;
        const XrXirCellProofBodyHead *function=&head;
        uint64_t count = (uint64_t)function->parameter_count+function->instruction_count;
        if ((function->parameter_count && !function->parameters) ||
            (function->operand_count && !function->operands))
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
        physical += count;
        if (count > UINT32_MAX || physical > UINT32_MAX)
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BUDGET);
        bool tracked = false;
        if (build->body->range) {
            for (uint32_t v=0;v<function->parameter_count;++v) {
                if (!cell_proof_work(build,3)) return false;
                XrXirType type=cell_proof_type(build,f,v);
                if (build->status!=XR_XIR_OK ||
                    !cell_proof_inventory_parameter(build,f,function,v,type,&tracked)) return false;
            }
            if (!cell_proof_work(build,sizeof(XrXirCellProofRunCursor))) return false;
            XrXirCellProofRunCursor cursor={.function=f};
            while (cursor.next<function->instruction_count) {
                XrXirCellProofRun run;
                if (!cell_proof_run(build,function,&cursor,&run) ||
                    !cell_proof_inventory_run(build,f,function,&run,&tracked)) return false;
            }
        } else {
            for (uint32_t v = 0; v < (uint32_t)count; ++v) {
                if (!cell_proof_work(build,3)) return false;
                XrXirType type=cell_proof_type(build,f,v);
                if (build->status!=XR_XIR_OK) return false;
                XrXirCellProofInstructionShape shape={0};
                const XrXirCellProofInstructionShape *instruction=NULL;
                if (v>=function->parameter_count) {
                    if (!cell_proof_work(build,sizeof(shape))) return false;
                    if (!cell_proof_shape(build,f,v-function->parameter_count,&shape)) return false;
                    XrXirType independent=cell_proof_type(build,f,v);
                    if (build->status!=XR_XIR_OK) return false;
                    if (type!=independent)
                        return cell_proof_error(build,f,v-function->parameter_count,XR_XIR_BAD_TYPE);
                    instruction=&shape;
                }
                if (!instruction) {
                    if (!cell_proof_inventory_parameter(build,f,function,v,type,&tracked)) return false;
                } else {
                    if (!cell_proof_inventory_value(build,f,function,v)) return false;
                    uint32_t i=v-function->parameter_count;
                    if (instruction->type!=type) return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
                    if (!cell_proof_inventory_instruction(build,f,i,type,instruction->op,&tracked)) return false;
                }
            }
        }
        if (!cell_proof_work(build,8)) return false;
        proof->functions[f] = (CellProofFunction){.begin=(uint32_t)values,
            .count=(uint32_t)count,.parameters=function->parameter_count,
            .stored=tracked?(uint32_t)count:0};
        if (tracked) values += count;
        if (values > UINT32_MAX)
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BUDGET);
    }
    return cell_proof_inventory_storage(build,values);
}


static bool cell_proof_role_row(CellProofBuild *build,uint32_t f,uint32_t i,const XrXirInstruction *op) {
    XrXirCellProvenance *proof=build->proof;const XirMetadataDomain *module=&build->body->domain;
    if (op->op!=XR_XIR_FUNCTION_REF && op->op!=XR_XIR_CALL &&
        op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_CLEANUP_REGISTER) return true;
    uint32_t target = (uint32_t)op->immediate;
    if (target >= module->function_count) return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
    CellProofFunction *callee = &proof->functions[target];
    if (op->args[1] > callee->parameters) return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
    if (op->op == XR_XIR_CLEANUP_REGISTER && (!module->declarations ||
        module->declarations->functions[target].cleanup_owner != f+1))
        return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
    if (!cell_proof_work(build,(uint64_t)op->args[1]*3)) return false;
    uint8_t use = op->op == XR_XIR_FUNCTION_REF ? 1u : op->op == XR_XIR_CLEANUP_REGISTER ? 4u : 2u;
    for (uint32_t p = 0; p < op->args[1]; ++p)
        if (cell_proof_tracking(proof,target,p) == 1) build->uses[callee->begin+p] |= use;
    if (op->op == XR_XIR_FUNCTION_REF) {
        const XrXirTypeNode *signature = xr_xir_callable_signature(module->types, op->type);
        if (!signature || signature->parameter_count != callee->parameters-op->args[1])
            return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
        if (!cell_proof_work(build,(uint64_t)signature->parameter_count*2)) return false;
        for (uint32_t p = 0; p < signature->parameter_count; ++p) {
            const XrXirCallableParameter *parameter = &signature->parameters[p];
            if (!xr_xir_callable_parameter_storage_valid(module->types, parameter))
                return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
            if (parameter->mode != XR_PARAM_REF) continue;
            if (op->args[1] || cell_proof_tracking(proof,target,p) != 1)
                return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
            /* A real unbound reference authorizes only the scoped call
             * protocol. No owner origin follows from its signature. */
            build->uses[callee->begin+p] |= 2u;
        }
    }
    return true;
}
/* Only real field invariance merges role-free reads. A zero-actual call still
 * checks its target and cleanup declaration. REF suffixes always replay. */
static bool cell_proof_roles_range(CellProofBuild *build,uint32_t f,
    const XrXirCellProofRun *run,bool *handled) {
    *handled=false;
    if (!build->body->range || run->varying&UINT32_C(1)) return true;
    if (!cell_proof_work(build,6)) return false;
    const XrXirInstruction *op=&run->operation;
    if (op->op!=XR_XIR_FUNCTION_REF && op->op!=XR_XIR_CALL &&
        op->op!=XR_XIR_INVOKE && op->op!=XR_XIR_CLEANUP_REGISTER) {*handled=true;return true;}
    if (op->op==XR_XIR_FUNCTION_REF || run->varying&UINT32_C(200) || op->args[1]) return true;
    if (!cell_proof_role_row(build,f,run->first,op)) return false;
    *handled=true;return true;
}

/* Only actual references assign a capture or lexical-cleanup role. */
static bool cell_proof_roles(CellProofBuild *build) {
    XrXirCellProvenance *proof = build->proof;
    const XirMetadataDomain *module = &build->body->domain;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        XrXirCellProofBodyHead head={0};if (!cell_proof_head(build,f,&head)) return false;
        const XrXirCellProofBodyHead *function=&head;
        if (!build->body->range) {
            for (uint32_t i=0;i<function->instruction_count;++i) {
                if (!cell_proof_work(build,1)) return false;
                XrXirInstruction operation={0};const XrXirInstruction *op=cell_proof_instruction(build,f,i,&operation);
                if (!op || !cell_proof_role_row(build,f,i,op)) return false;
            }
            continue;
        }
        if (!cell_proof_work(build,sizeof(XrXirCellProofRunCursor))) return false;
        XrXirCellProofRunCursor cursor={.function=f};
        while (cursor.next<function->instruction_count) {
            XrXirCellProofRun run;bool handled=false;
            if (!cell_proof_run(build,function,&cursor,&run) ||
                !cell_proof_roles_range(build,f,&run,&handled)) return false;
            if (handled) continue;
            for (uint32_t i=run.first;i-run.first<run.count;++i) {
                if (!cell_proof_work(build,1)) return false;
                XrXirInstruction operation;const XrXirInstruction *op=
                    cell_proof_run_operation(build,f,&run,i,&operation);
                if (!op || !cell_proof_role_row(build,f,i,op)) return false;
            }
        }
    }
    size_t words = 0; uint64_t scopes = 0;
    for (uint32_t f = 0; f < module->function_count; ++f) {
        CellProofFunction *function = &proof->functions[f];
        function->scope_begin = (uint32_t)scopes;
        if (!cell_proof_work(build,(uint64_t)function->parameters*5+3)) return false;
        for (uint32_t p = 0; p < function->parameters; ++p) {
            uint32_t id = function->begin+p;
            if (cell_proof_tracking(proof,f,p) != 1) continue;
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
        if (function->words && (size_t)function->stored > (SIZE_MAX-words)/function->words)
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BUDGET);
        words += (size_t)function->stored*function->words;
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
            if (!cell_proof_work(build,2)) return false;
            uint32_t id = function->begin+p;
            if (cell_proof_tracking(proof,f,p) != 1 || proof->roles[id] == XR_XIR_CELL_PROOF_OWNED_CAPTURE) continue;
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
/* One finite path walker names the real root place. Class facts describe
 * its actual SSA subject; they do not grant a borrow or execution permission. */
static bool cell_proof_project_root(CellProofBuild *build,uint32_t f,uint32_t value,
    uint32_t *output) {
    XrXirCellProofBodyHead head={0};if (!cell_proof_head(build,f,&head)) return false;
    const XrXirCellProofBodyHead *function=&head;uint32_t depth=0;
    for (;;) {
        if (!cell_proof_work(build,1)) return false;
        if (value<function->parameter_count || value-function->parameter_count>=function->instruction_count)
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
        XrXirInstruction operation={0};const XrXirInstruction *op=cell_proof_instruction(build,f,value-function->parameter_count,&operation);
        if (!op) return false;
        if (op->op==XR_XIR_OBJECT_PLACE || op->op==XR_XIR_CELL_PLACE) {
            if (op->op==XR_XIR_OBJECT_PLACE && (op->args[0]>=build->proof->functions[f].count ||
                !xr_xir_type_is_class(build->body->domain.types,cell_proof_type(build,f,op->args[0])) ||
                op->type!=cell_proof_type(build,f,op->args[0])))
                return cell_proof_error(build,f,value-function->parameter_count,XR_XIR_BAD_TYPE);
            *output=value;return true;
        }
        if ((op->op!=XR_XIR_FIELD_PLACE && op->op!=XR_XIR_INDEX_PLACE) ||
            ++depth>function->instruction_count)
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
        value=op->args[0];
    }
}

/* The existing uses byte records exact projected OBJECT_PLACE membership.
 * Bit8 cannot overlap parameter capture/scoped/cleanup uses1/2/4. */
static bool cell_proof_class_collect(CellProofBuild *build) {
    if (!build->proof->project_count) return true;
    for (uint32_t f=0;f<build->body->domain.function_count;++f) {
        XrXirCellProofBodyHead head={0};if (!cell_proof_head(build,f,&head)) return false;
        const XrXirCellProofBodyHead *function=&head;
        for (uint32_t i=0;i<function->instruction_count;++i) {
            if (!cell_proof_work(build,1)) return false;
            XrXirInstruction operation={0};const XrXirInstruction *op=cell_proof_instruction(build,f,i,&operation);
            if (!op) return false;
            if (op->op!=XR_XIR_CELL_PROJECT) continue;
            uint32_t root=0;
            if (!cell_proof_project_root(build,f,op->args[0],&root)) return false;
            XrXirInstruction root_operation={0};const XrXirInstruction *root_op=
                cell_proof_instruction(build,f,root-function->parameter_count,&root_operation);
            if (!root_op) return false;
            if (root_op->op!=XR_XIR_OBJECT_PLACE) continue;
            if (!cell_proof_work(build,2)) return false;
            build->uses[build->proof->functions[f].begin+root]|=8u;
        }
    }
    return true;
}

/* Every record and bit matrix belongs to this proof's physical function
 * domain. No producer array, Class object or type arena remains borrowed. */
static bool cell_proof_class_storage(CellProofBuild *build) {
    XrXirCellProvenance *proof=build->proof;size_t words=0;uint64_t roots=0;
    if (!proof->project_count) return true;
    for (uint32_t f=0;f<proof->function_count;++f) {
        CellProofFunction *function=&proof->functions[f];function->class_begin=(uint32_t)roots;
        for (uint32_t v=0;v<function->stored;++v) {
            if (!cell_proof_work(build,1)) return false;
            if (!(build->uses[function->begin+v]&8u)) continue;
            if (!cell_proof_work(build,1)) return false;
            ++function->class_count;
        }
        roots+=function->class_count;
        if (!cell_proof_work(build,6)) return false;
        function->class_words=function->class_count/64u+(function->class_count%64u!=0);
        function->class_dependency_begin=words;
        if (roots>UINT32_MAX || (function->class_words &&
            function->stored>(SIZE_MAX-words)/function->class_words))
            return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BUDGET);
        words+=(size_t)function->stored*function->class_words;
    }
    if (roots>SIZE_MAX/sizeof(*proof->class_roots) || words>SIZE_MAX/sizeof(uint64_t))
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BUDGET);
    proof->class_count=(uint32_t)roots;proof->class_dependency_count=words;
    proof->class_roots=xir_compile_calloc(build->context,(size_t)roots,sizeof(*proof->class_roots),&build->status);
    proof->class_dependencies=xir_compile_calloc(build->context,words,sizeof(uint64_t),&build->status);
    if (build->status!=XR_XIR_OK) return false;
    for (uint32_t f=0;f<proof->function_count;++f) {
        const CellProofFunction *stored=&proof->functions[f];uint32_t at=0;
        XrXirCellProofBodyHead head={0};if (!cell_proof_head(build,f,&head)) return false;
        const XrXirCellProofBodyHead *function=&head;
        for (uint32_t v=0;v<stored->stored;++v) {
            if (!cell_proof_work(build,1)) return false;
            if (!(build->uses[stored->begin+v]&8u)) continue;
            if (v<function->parameter_count || at>=stored->class_count)
                return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
            XrXirInstruction operation={0};const XrXirInstruction *op=cell_proof_instruction(build,f,v-function->parameter_count,&operation);
            if (!op) return false;
            if (op->op!=XR_XIR_OBJECT_PLACE || op->args[0]>=stored->count ||
                !xr_xir_type_is_class(build->body->domain.types,cell_proof_type(build,f,op->args[0])))
                return cell_proof_error(build,f,v-function->parameter_count,XR_XIR_BAD_TYPE);
            if (!cell_proof_work(build,sizeof(*proof->class_roots))) return false;
            proof->class_roots[stored->class_begin+at++]=(XrXirCellClassRoot){v,op->args[0]};
            if (!cell_proof_work(build,1)) return false;
            proof->tracked[stored->begin+op->args[0]]|=4u;
        }
        if (at!=stored->class_count) return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    }
    return true;
}

static bool cell_proof_class_seed(CellProofBuild *build,uint32_t f,uint32_t place) {
    XrXirCellProvenance *proof=build->proof;const CellProofFunction *function=&proof->functions[f];
    uint32_t low=0,high=function->class_count;
    while (low<high) {
        if (!cell_proof_work(build,3)) return false;
        uint32_t middle=low+(high-low)/2;
        if (proof->class_roots[function->class_begin+middle].place<place) low=middle+1;
        else high=middle;
    }
    if (!cell_proof_work(build,2)) return false;
    if (low>=function->class_count || proof->class_roots[function->class_begin+low].place!=place)
        return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    uint64_t *row=cell_proof_class_dependencies(proof,function->begin+place);
    uint64_t bit=UINT64_C(1)<<(low%64u);
    if (row[low/64u]&bit) return true;
    if (!cell_proof_work(build,1)) return false;
    row[low/64u]|=bit;return cell_proof_enqueue(build,function->begin+place);
}
static bool cell_proof_project_identity(CellProofBuild *build,uint32_t f,uint32_t value) {
    uint32_t root=0;
    if (!cell_proof_project_root(build,f,value,&root)) return false;
    XrXirCellProofBodyHead head={0};if (!cell_proof_head(build,f,&head)) return false;
    const XrXirCellProofBodyHead *function=&head;
    XrXirInstruction operation={0};const XrXirInstruction *op=
        cell_proof_instruction(build,f,root-function->parameter_count,&operation);
    if (!op) return false;
    if (op->op!=XR_XIR_OBJECT_PLACE) return true;
    return cell_proof_seed(build,build->proof->functions[f].begin+root,XR_XIR_CELL_ORIGIN_ROOT_UNRESOLVED) &&
        cell_proof_class_seed(build,f,root);
}
static bool cell_proof_local_uninit(CellProofBuild *build,uint32_t f,uint32_t i) {
    XrXirInstruction operation={0};const XrXirInstruction *op=cell_proof_instruction(build,f,i,&operation);
    return op && op->op==XR_XIR_LOCAL_UNINIT;
}

/* This exact row receiver is shared by edge count and edge write. */
static bool cell_proof_edge_row(CellProofBuild *build,uint32_t f,uint32_t i,
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
            !cell_proof_edge(build,begin+op->args[0],result,false)) return false;
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
            if (!cell_proof_edge(build,source,result,true)) return false;
        } else if (op->op == XR_XIR_FUNCTION_REF) {
            uint32_t target = (uint32_t)op->immediate;
            uint32_t formal = proof->functions[target].begin+a;
            if (cell_proof_tracking(proof,target,a) == 1) {
                if (!proof->functions[f].stored)
                    return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
                if (!cell_proof_edge(build,source,formal,true)) return false;
            }
        }
        if ((op->op == XR_XIR_LOCAL_WRITE || op->op == XR_XIR_OWNED_LOCAL_WRITE) && a == 1) {
            uint32_t destination = begin+op->args[0];
            if (op->args[0] >= proof->functions[f].count)
                return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
            if (cell_proof_tracking(proof,f,op->args[0]) == 1 &&
                !cell_proof_edge(build,source,destination,true)) return false;
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
/* Dynamic Cell and Slot payloads must read their actual source metadata.
 * Every other operand count is computed by the original metadata core. */
static bool cell_proof_zero_operands(CellProofBuild *build,uint32_t f,
    const XrXirCellProofBodyHead *head,const XrXirCellProofRun *run,bool *zero) {
    *zero=false;
    if (!build->body->range || run->varying&UINT32_C(1)) return true;
    const XrXirInstruction *op=&run->operation;
    if (!cell_proof_work(build,9)) return false;
    if (op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_READ || op->op==XR_XIR_CELL_WRITE ||
        op->op==XR_XIR_CELL_LOCAL_WRITE || op->op==XR_XIR_SLOT_INIT || op->op==XR_XIR_SLOT_STORE)
        return true;
    if (xr_xir_op_uses_operand_table(op->op) && run->varying&UINT32_C(8)) return true;
    uint32_t count=cell_proof_operand_count(build,f,head,op);
    if (build->status!=XR_XIR_OK) return false;
    *zero=!count;return true;
}
/* This is absence of an action in this walker, never absence of authority.
 * Place roots and even zero-payload Cell producers retain the row receiver. */
static bool cell_proof_edges_range(CellProofBuild *build,uint32_t f,
    const XrXirCellProofBodyHead *head,const XrXirCellProofRun *run,bool *handled) {
    *handled=false;bool zero=false;
    if (!cell_proof_zero_operands(build,f,head,run,&zero) || !zero) return build->status==XR_XIR_OK;
    if (!cell_proof_work(build,7)) return false;
    const XrXirInstruction *op=&run->operation;
    if (run->varying&UINT32_C(2) || cell_proof_cell(&build->body->domain,op->type) ||
        cell_proof_place(op->op) || op->op==XR_XIR_CELL_PROJECT || op->op==XR_XIR_CELL_LOCAL_WRITE)
        return true;
    if (xr_xir_op_uses_operand_table(op->op)) {
        if (run->varying&UINT32_C(4)) return true;
        if (op->args[0]>head->operand_count) return cell_proof_error(build,f,run->first,XR_XIR_BAD_STRUCTURE);
    }
    *handled=true;return true;
}

static bool cell_proof_build_edges(CellProofBuild *build) {
    for (uint32_t f=0;f<build->body->domain.function_count;++f) {
        XrXirCellProofBodyHead head={0};if (!cell_proof_head(build,f,&head)) return false;
        if (!build->body->range) {
            for (uint32_t i=0;i<head.instruction_count;++i) {
                if (!cell_proof_work(build,3)) return false;
                XrXirInstruction operation={0};const XrXirInstruction *op=cell_proof_instruction(build,f,i,&operation);
                if (!op || !cell_proof_edge_row(build,f,i,&head,op)) return false;
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
                    cell_proof_run_operation(build,f,&run,i,&operation);
                if (!op || !cell_proof_edge_row(build,f,i,&head,op)) return false;
            }
        }
    }
    return true;
}

static bool cell_proof_edge_storage_bytes(size_t count, size_t *bytes) {
    if (count > SIZE_MAX/sizeof(CellProofEdge)) return false;
    *bytes = count*sizeof(CellProofEdge); return true;
}
/* The same complete walker seeds one proof and counts every actual edge.
 * Replaying it appends the exact ordered graph; seed OR operations are idempotent.
 * No solver or output observes the proof before both finite passes complete. */
static bool cell_proof_prepare_edges(CellProofBuild *build) {
    build->counting_edges = true;
    bool counted = cell_proof_build_edges(build);
    build->counting_edges = false;
    if (!counted) return false;
    build->edge_capacity = build->edge_count; build->edge_count = 0;
    size_t bytes = 0;
    if (!cell_proof_edge_storage_bytes(build->edge_capacity,&bytes))
        return cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BUDGET);
    if (!cell_proof_work(build,4)) return false;
    build->edges = xir_compile_alloc(build->context,bytes,&build->status);
    if (build->status != XR_XIR_OK || !cell_proof_build_edges(build)) return false;
    if (!cell_proof_work(build,1)) return false;
    return build->edge_count == build->edge_capacity ||
        cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
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
    if (proof->value_functions[from]==proof->value_functions[to]) {
        uint32_t words=proof->functions[proof->value_functions[from]].class_words;
        const uint64_t *source=cell_proof_class_dependencies(proof,from);
        uint64_t *destination=cell_proof_class_dependencies(proof,to);
        for (uint32_t w=0;w<words;++w) {
            if (!cell_proof_work(build,3)) return false;
            uint64_t joined=source[w]|destination[w];
            if (joined!=destination[w]) {
                if (!cell_proof_work(build,1)) return false;
                destination[w]=joined;changed=true;
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

static bool cell_proof_view_root(CellProofBuild *build,uint32_t f,uint32_t i,uint32_t source) {
    XrXirCellProvenance *proof=build->proof;const CellProofFunction *function=&proof->functions[f];
    const uint64_t *dependencies=cell_proof_dependencies(proof,source);
    if (!dependencies && function->scopes) return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
    for (uint32_t p=0;p<function->scopes;++p) {
        if (!cell_proof_work(build,2)) return false;
        if (!(dependencies[p/64u]&(UINT64_C(1)<<(p%64u)))) continue;
        uint32_t parameter=proof->scope_parameters[function->scope_begin+p];
        if (parameter>=function->parameters) return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
        uint8_t role=proof->roles[function->begin+parameter];
        /* A scoped origin bit alone includes unbound definition parameters.
         * Only actual call/reference or lexical-cleanup edges bind a scope. */
        if (role==XR_XIR_CELL_PROOF_UNKNOWN) return cell_proof_error(build,f,i,XR_XIR_UNRESOLVED);
        if (role!=XR_XIR_CELL_PROOF_SCOPED_REF && role!=XR_XIR_CELL_PROOF_LEXICAL_CLEANUP)
            return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
    }
    return true;
}
static bool cell_proof_sink(CellProofBuild *build, uint32_t f, uint32_t i,
    uint32_t ordinal, uint32_t source,const XrXirInstruction *op) {
    if (!op) return false;
    XrXirCellProvenance *proof = build->proof;
    uint8_t origin = proof->origins[source];
    if (!(origin & (XR_XIR_CELL_ORIGIN_OWNED | XR_XIR_CELL_ORIGIN_SCOPED | XR_XIR_CELL_ORIGIN_MODULE)) ||
        (origin & XR_XIR_CELL_ORIGIN_UNKNOWN)) return cell_proof_error(build,f,i,XR_XIR_BAD_VALUE);
    if (cell_proof_identity_producer(op->op)) return true;
    if (op->op==XR_XIR_VIEW_NEW && !ordinal) return cell_proof_view_root(build,f,i,source);
    if (op->op == XR_XIR_CELL_STRING_APPEND && !ordinal)
        return !(origin & XR_XIR_CELL_ORIGIN_SCOPED) || cell_proof_error(build,f,i,XR_XIR_BAD_VALUE);
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
        XrXirType callee = cell_proof_type(build,f,(uint32_t)op->immediate);
        const XrXirTypeNode *signature = xr_xir_callable_signature(build->body->domain.types,callee);
        if (!signature || ordinal >= signature->parameter_count)
            return cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
        const XrXirCallableParameter *parameter = &signature->parameters[ordinal];
        return (xr_xir_callable_parameter_storage_valid(build->body->domain.types,parameter) &&
            parameter->mode == XR_PARAM_REF && parameter->type ==
                cell_proof_type(build,f,source-proof->functions[f].begin)) ||
            cell_proof_error(build,f,i,XR_XIR_BAD_TYPE);
    }
    if (op->op == XR_XIR_SLOT_INIT || op->op == XR_XIR_SLOT_GROUP_INIT)
        return origin == (XR_XIR_CELL_ORIGIN_OWNED | XR_XIR_CELL_ORIGIN_MODULE) ||
            cell_proof_error(build,f,i,XR_XIR_BAD_VALUE);
    /* Return, aggregate, capture, GO and unsupported retention fail closed. */
    return cell_proof_error(build,f,i,XR_XIR_BAD_VALUE);
}

static bool cell_proof_validate_row(CellProofBuild *build,uint32_t f,uint32_t i,
    const XrXirCellProofBodyHead *function,const XrXirInstruction *op) {
    XrXirCellProvenance *proof=build->proof;uint32_t begin=proof->functions[f].begin;
    uint32_t count = cell_proof_operand_count(build,f,function,op);
    for (uint32_t a = 0; a < count; ++a) {
        if (!cell_proof_work(build,4)) return false;
        if (op->op == XR_XIR_PHI && !(a & 1u)) continue;
        uint32_t value = cell_proof_operand(function,op,a);
        if (value >= proof->functions[f].count)
            return cell_proof_error(build,f,i,XR_XIR_BAD_STRUCTURE);
        uint32_t source = begin+value;
        if (cell_proof_tracking(proof,f,value) == 1 && !cell_proof_sink(build,f,i,a,source,op)) return false;
    }
    return true;
}

static bool cell_proof_validate(CellProofBuild *build) {
    XrXirCellProvenance *proof = build->proof;
    for (uint32_t f = 0; f < build->body->domain.function_count; ++f) {
        XrXirCellProofBodyHead head={0};if (!cell_proof_head(build,f,&head)) return false;
        const XrXirCellProofBodyHead *function=&head;
        uint32_t begin = proof->functions[f].begin;
        for (uint32_t p = 0; p < function->parameter_count; ++p) {
            if (!cell_proof_work(build,3)) return false;
            if (proof->functions[f].stored && proof->roles[begin+p] == XR_XIR_CELL_PROOF_OWNED_CAPTURE &&
                proof->origins[begin+p] != XR_XIR_CELL_ORIGIN_OWNED)
                return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_VALUE);
        }
        if (!build->body->range) {
            for (uint32_t i=0;i<function->instruction_count;++i) {
                if (!cell_proof_work(build,1)) return false;
                XrXirInstruction operation={0};const XrXirInstruction *op=cell_proof_instruction(build,f,i,&operation);
                if (!op || !cell_proof_validate_row(build,f,i,function,op)) return false;
            }
            continue;
        }
        if (!cell_proof_work(build,sizeof(XrXirCellProofRunCursor))) return false;
        XrXirCellProofRunCursor cursor={.function=f};
        while (cursor.next<function->instruction_count) {
            XrXirCellProofRun run;bool zero=false;
            if (!cell_proof_run(build,function,&cursor,&run) ||
                !cell_proof_zero_operands(build,f,function,&run,&zero)) return false;
            if (zero) continue;
            for (uint32_t i=run.first;i-run.first<run.count;++i) {
                if (!cell_proof_work(build,1)) return false;
                XrXirInstruction operation;const XrXirInstruction *op=
                    cell_proof_run_operation(build,f,&run,i,&operation);
                if (!op || !cell_proof_validate_row(build,f,i,function,op)) return false;
            }
        }
    }
    return true;
}

static XrXirStatus cell_proof_resident_bytes(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof,uint64_t *output) {
    uint64_t words=0,total=sizeof(*proof);
    for (uint32_t f=0;f<proof->function_count;++f) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        const CellProofFunction *function=proof->functions+f;
        if (function->words && function->stored>(UINT64_MAX-words)/function->words) return XR_XIR_BUDGET;
        words+=(uint64_t)function->stored*function->words;
    }
    uint64_t count[]={proof->function_count,proof->value_count,proof->scope_count,words,proof->class_count,proof->class_dependency_count};
    uint64_t stride[]={sizeof(*proof->functions),3u+sizeof(uint32_t),sizeof(uint32_t),sizeof(uint64_t),sizeof(*proof->class_roots),sizeof(uint64_t)};
    for (uint32_t p=0;p<6;++p) {
        if (!xir_compile_work(context,1)) return XR_XIR_BUDGET;
        if (count[p]>(UINT64_MAX-total)/stride[p]) return XR_XIR_BUDGET;total+=count[p]*stride[p];
    }
    *output=total;return XR_XIR_OK;
}
XR_FUNC void xr_xir_compile_cell_provenance_free(XrXirCellProvenance *proof) {
    if (!proof) return;
    xr_compile_resources_free(proof->functions); xr_compile_resources_free(proof->origins);
    xr_compile_resources_free(proof->roles); xr_compile_resources_free(proof->tracked);
    xr_compile_resources_free(proof->scope_parameters); xr_compile_resources_free(proof->value_functions);
    xr_compile_resources_free(proof->dependencies);xr_compile_resources_free(proof->class_roots);
    xr_compile_resources_free(proof->class_dependencies);xr_compile_resources_free(proof);
}
static void cell_proof_complete(CellProofBuild *build) {
    /* Full inventory proves that no physical Cell or access place exists.
     * The verified caller still owns every call, signature and SSA check. */
    if (build->status==XR_XIR_OK && build->proof->value_count &&
        cell_proof_class_collect(build) && cell_proof_class_storage(build) && cell_proof_roles(build) &&
        cell_proof_prepare_edges(build) && cell_proof_solve(build)) {
        /* Bottom is not ownership. Unrooted cycles taint every consumer before admission. */
        for (uint32_t v = 0; v < build->proof->value_count && build->status == XR_XIR_OK; ++v) {
            if (!cell_proof_work(build,2)) break;
            if (build->proof->tracked[v] == 1 && !build->proof->origins[v])
                cell_proof_seed(build,v,XR_XIR_CELL_ORIGIN_UNKNOWN);
        }
        if (build->status == XR_XIR_OK && cell_proof_solve(build)) cell_proof_validate(build);
    }
}
static void cell_proof_workspace_free(CellProofBuild *build) {
    xr_compile_resources_free(build->heads);xr_compile_resources_free(build->reverse_heads);
    xr_compile_resources_free(build->queue);xr_compile_resources_free(build->queued);
    xr_compile_resources_free(build->uses);xr_compile_resources_free(build->edges);
}
static XrXirStatus cell_proof_derive(const XrXirCompileContext *context,
    const XrXirCellProofBodyRead *body, XrXirCellProvenance **output, XrXirDiagnostic *diagnostic) {
    if (!cell_proof_body_valid(context,body) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    CellProofBuild build = {0}; build.context = context; build.body = body;
    build.location = (XrXirDiagnostic){XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    if (cell_proof_inventory(&build)) cell_proof_complete(&build);
    cell_proof_workspace_free(&build);
    build.location.status = build.status;
    if (diagnostic) *diagnostic = build.location;
    if (build.status != XR_XIR_OK) xr_xir_compile_cell_provenance_free(build.proof);
    else *output = build.proof;
    return build.status;
}
struct XrXirCellProofBuilder {
    CellProofBuild build;
    XrXirCellProofBodyRead body;
    XrXirCellProofBodyHead head;
    XrXirCellProofSpanCursor span;
    uint64_t values,physical;
    uint32_t function,value;
    bool open,tracked;
};
XR_FUNC void xir_cell_proof_builder_free(XrXirCellProofBuilder *builder) {
    if (!builder) return;
    cell_proof_workspace_free(&builder->build);
    xr_xir_compile_cell_provenance_free(builder->build.proof);
    xr_compile_resources_free(builder);
}
XR_FUNC XrXirStatus xir_cell_proof_builder_begin(const XrXirCompileContext *context,
    const XrXirCellProofBodyRead *body,XrXirCellProofBuilder **output) {
    if (!cell_proof_body_valid(context,body) || !output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    XrXirCellProofBuilder *builder=xir_compile_calloc(context,1,sizeof(*builder),&status);
    if (!builder) return status;
    if (!xir_compile_work(context,sizeof(builder->body)+sizeof(builder->build.context)+
        sizeof(builder->build.body)+sizeof(builder->build.location))) {
        xir_cell_proof_builder_free(builder);return XR_XIR_BUDGET;
    }
    builder->body=*body;builder->build.context=context;builder->build.body=&builder->body;
    builder->build.location=(XrXirDiagnostic){XR_XIR_OK,UINT32_MAX,UINT32_MAX,UINT32_MAX,XR_XIR_DIAGNOSTIC_NONE};
    if (!cell_proof_inventory_begin(&builder->build)) {
        status=builder->build.status;xir_cell_proof_builder_free(builder);return status;
    }
    *output=builder;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_cell_proof_builder_head(XrXirCellProofBuilder *builder,
    uint32_t f,const XrXirCellProofBodyHead *head) {
    if (!builder) return XR_XIR_BAD_STRUCTURE;
    CellProofBuild *build=&builder->build;
    if (build->status!=XR_XIR_OK) return build->status;
    if (!head || builder->open || f!=builder->function || f>=build->body->domain.function_count ||
        (head->parameter_count && !head->parameters) || (head->operand_count && !head->operands)) {
        cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);return build->status;
    }
    uint64_t count=(uint64_t)head->parameter_count+head->instruction_count;
    if (count>UINT32_MAX || count>UINT32_MAX-builder->physical) {
        cell_proof_error(build,f,UINT32_MAX,XR_XIR_BUDGET);return build->status;
    }
    if (!cell_proof_work(build,6+sizeof(*head)+sizeof(builder->physical)+
        sizeof(builder->value)+sizeof(builder->open)+sizeof(builder->tracked))) return build->status;
    if (builder->body.span) {
        if (!cell_proof_work(build,sizeof(builder->span))) return build->status;
        builder->span=(XrXirCellProofSpanCursor){.function=f};
    }
    builder->physical+=count;builder->head=*head;builder->value=0;
    builder->open=true;builder->tracked=false;return XR_XIR_OK;
}
static XrXirStatus cell_proof_builder_begin_row(XrXirCellProofBuilder *builder,
    uint32_t f,uint32_t value) {
    if (!builder) return XR_XIR_BAD_STRUCTURE;
    CellProofBuild *build=&builder->build;
    if (build->status!=XR_XIR_OK) return build->status;
    if (!builder->open || f!=builder->function || value!=builder->value) {
        cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);return build->status;
    }
    if (!cell_proof_work(build,3+sizeof(builder->value))) return build->status;
    return XR_XIR_OK;
}
/* Distinct real row kinds share range/type/op inventory without a caller
 * verified flag. Every new entry and kernel has at most six parameters. */
XR_FUNC XrXirStatus xir_cell_proof_builder_parameter(XrXirCellProofBuilder *builder,
    uint32_t f,uint32_t value,XrXirType type) {
    XrXirStatus status=cell_proof_builder_begin_row(builder,f,value);
    if (status!=XR_XIR_OK) return status;
    CellProofBuild *build=&builder->build;
    if (!cell_proof_inventory_parameter(build,f,&builder->head,value,type,&builder->tracked))
        return build->status;
    ++builder->value;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_cell_proof_builder_instruction(XrXirCellProofBuilder *builder,
    uint32_t f,uint32_t value,XrXirType type,XrXirOp opcode,XrXirType actual_type) {
    XrXirStatus status=cell_proof_builder_begin_row(builder,f,value);
    if (status!=XR_XIR_OK) return status;
    CellProofBuild *build=&builder->build;
    if (!cell_proof_inventory_value(build,f,&builder->head,value)) return build->status;
    if (value<builder->head.parameter_count || actual_type!=type) {
        cell_proof_error(build,f,value<builder->head.parameter_count?UINT32_MAX:
            value-builder->head.parameter_count,XR_XIR_BAD_TYPE);return build->status;
    }
    uint32_t i=value-builder->head.parameter_count;
    if (!cell_proof_inventory_instruction(build,f,i,type,opcode,&builder->tracked)) return build->status;
    ++builder->value;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_cell_proof_builder_span(XrXirCellProofBuilder *builder,
    uint32_t f,uint32_t first,uint32_t count,const XrXirCellProofInstructionShape *source) {
    if (!builder) return XR_XIR_BAD_STRUCTURE;
    CellProofBuild *build=&builder->build;
    if (build->status!=XR_XIR_OK) return build->status;
    if (!builder->open || f!=builder->function || !source || !count || !builder->body.span ||
        first>builder->head.instruction_count || count>builder->head.instruction_count-first ||
        (uint64_t)builder->head.parameter_count+first!=builder->value ||
        source->op<=XR_XIR_INVALID || source->op>=XR_XIR_OP_COUNT) {
        cell_proof_error(build,f,first,XR_XIR_BAD_STRUCTURE);return build->status;
    }
    if (!cell_proof_work(build,3+sizeof(builder->value))) return build->status;
    XrXirStatus status=builder->body.span(build->context,builder->body.owner,f,first,count,&builder->span);
    if (status!=XR_XIR_OK) { cell_proof_error(build,f,first,status);return build->status; }
    if (!cell_proof_work(build,4)) return build->status;
    if (builder->span.shape.op!=source->op || builder->span.shape.type!=source->type ||
        !xir_cell_proof_scalar_shape(builder->span.shape.op,builder->span.shape.type)) {
        cell_proof_error(build,f,first,XR_XIR_BAD_TYPE);return build->status;
    }
    if (!cell_proof_inventory_instruction(build,f,first,builder->span.shape.type,
        builder->span.shape.op,&builder->tracked)) return build->status;
    builder->value+=count;return XR_XIR_OK;
}
static bool cell_proof_builder_span_end(XrXirCellProofBuilder *builder,uint32_t f) {
    CellProofBuild *build=&builder->build;
    if (!cell_proof_work(build,sizeof(XrXirCellProofBodyHead))) return false;
    XrXirCellProofBodyHead head={0};
    XrXirStatus status=builder->body.head(build->context,builder->body.owner,f,&head);
    if (status!=XR_XIR_OK) return cell_proof_error(build,f,UINT32_MAX,status);
    if (!cell_proof_work(build,7)) return false;
    if (head.parameter_count!=builder->head.parameter_count || head.result!=builder->head.result ||
        head.instruction_count!=builder->head.instruction_count || head.operand_count!=builder->head.operand_count ||
        (head.parameter_count && (!head.parameters || !builder->head.parameters)) ||
        (head.operand_count && (!head.operands || !builder->head.operands)))
        return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    for (uint32_t p=0;p<head.parameter_count;++p) {
        if (!cell_proof_work(build,2)) return false;
        if (head.parameters[p]!=builder->head.parameters[p]) return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_TYPE);
    }
    for (uint32_t a=0;a<head.operand_count;++a) {
        if (!cell_proof_work(build,2)) return false;
        if (head.operands[a]!=builder->head.operands[a]) return cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    }
    status=builder->body.span(build->context,builder->body.owner,f,head.instruction_count,0,&builder->span);
    return status==XR_XIR_OK || cell_proof_error(build,f,UINT32_MAX,status);
}
XR_FUNC XrXirStatus xir_cell_proof_builder_function(XrXirCellProofBuilder *builder,uint32_t f) {
    if (!builder) return XR_XIR_BAD_STRUCTURE;
    CellProofBuild *build=&builder->build;
    if (build->status!=XR_XIR_OK) return build->status;
    uint64_t count=(uint64_t)builder->head.parameter_count+builder->head.instruction_count;
    if (!builder->open || f!=builder->function || builder->value!=count) {
        cell_proof_error(build,f,UINT32_MAX,XR_XIR_BAD_STRUCTURE);return build->status;
    }
    if (builder->body.span) {
        if (!cell_proof_work(build,2)) return build->status;
        /* Only a successful numeric span advance needs the new prefix EOF
         * check. A nonempty scalar function still completes its original
         * full writer; an empty function has no scalar row to prove EOF. */
        if ((!builder->head.instruction_count || builder->span.next) &&
            !cell_proof_builder_span_end(builder,f)) return build->status;
    }
    if (builder->tracked && count>UINT32_MAX-builder->values) {
        cell_proof_error(build,f,UINT32_MAX,XR_XIR_BUDGET);return build->status;
    }
    if (!cell_proof_work(build,8+sizeof(builder->values)+sizeof(builder->function)+sizeof(builder->open)))
        return build->status;
    build->proof->functions[f]=(CellProofFunction){.begin=(uint32_t)builder->values,
        .count=(uint32_t)count,.parameters=builder->head.parameter_count,
        .stored=builder->tracked?(uint32_t)count:0};
    if (builder->tracked) builder->values+=count;
    ++builder->function;builder->open=false;return XR_XIR_OK;
}
XR_FUNC XrXirStatus xir_cell_proof_builder_finish(XrXirCellProofBuilder **input,
    XrXirCellProvenance **output) {
    if (!input || !*input || !output || *output) return XR_XIR_BAD_STRUCTURE;
    XrXirCellProofBuilder *builder=*input;CellProofBuild *build=&builder->build;
    if (build->status==XR_XIR_OK && (builder->open ||
        builder->function!=build->body->domain.function_count))
        cell_proof_error(build,UINT32_MAX,UINT32_MAX,XR_XIR_BAD_STRUCTURE);
    if (build->status==XR_XIR_OK && cell_proof_inventory_storage(build,builder->values)) cell_proof_complete(build);
    XrXirStatus status=build->status;
    if (status==XR_XIR_OK) { *output=build->proof;build->proof=NULL; }
    xir_cell_proof_builder_free(builder);*input=NULL;return status;
}

XR_FUNC XrXirStatus xr_xir_compile_cell_provenance_verified(const XrXirCompileContext *context,
    const XrXirModule *module,XrXirCellProvenance **output,XrXirDiagnostic *diagnostic) {
    if (!xir_compile_context_valid(context) || !module || !output || *output ||
        (module->function_count && !module->functions)) return XR_XIR_BAD_STRUCTURE;
    XrXirCellProofBodyRead body=cell_proof_raw_body(context,module);
    return cell_proof_derive(context,&body,output,diagnostic);
}
XR_FUNC XrXirStatus xr_xir_compile_cell_provenance_body_matches(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof,const XrXirCellProofBodyRead *body,XrXirDiagnostic *diagnostic) {
    if (!cell_proof_body_valid(context,body) || !proof || proof->resources!=context->resources)
        return XR_XIR_BAD_STRUCTURE;
    XrXirCellProvenance *fresh=NULL;XrXirDiagnostic location={0};
    XrXirStatus status=cell_proof_derive(context,body,&fresh,&location);
    if (status==XR_XIR_OK) status=cell_proof_matches(context,proof,fresh);
    xr_xir_compile_cell_provenance_free(fresh);
    location.status=status;if (diagnostic) *diagnostic=location;
    return status;
}

XR_FUNC uint32_t xr_xir_cell_provenance_origin(const XrXirCellProvenance *proof,
    uint32_t function, uint32_t value) {
    if (!proof || function >= proof->function_count || value >= proof->functions[function].count)
        return XR_XIR_CELL_ORIGIN_UNKNOWN;
    const CellProofFunction *f = &proof->functions[function];
    return f->stored ? proof->origins[f->begin+value] : 0;
}
XR_FUNC XrXirCellProofRole xr_xir_cell_provenance_role(const XrXirCellProvenance *proof,
    uint32_t function, uint32_t parameter) {
    if (!proof || function >= proof->function_count || parameter >= proof->functions[function].parameters)
        return XR_XIR_CELL_PROOF_UNKNOWN;
    const CellProofFunction *f = &proof->functions[function];
    return f->stored ? (XrXirCellProofRole)proof->roles[f->begin+parameter] : XR_XIR_CELL_PROOF_UNKNOWN;
}
XR_FUNC XrXirStatus xr_xir_compile_cell_origin_view(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof, uint32_t function, uint32_t value, XrXirCellOriginView *output) {
    if (!xir_compile_context_valid(context) || !proof || proof->resources != context->resources ||
        !output || function >= proof->function_count ||
        value >= proof->functions[function].count) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(context,8)) return XR_XIR_BUDGET;
    const CellProofFunction *f = &proof->functions[function]; uint32_t node = f->begin+value;
    uint8_t origin = f->stored ? proof->origins[node] : 0;
    uint32_t intrinsic = (origin & XR_XIR_CELL_ORIGIN_MODULE ? XR_XIR_CELL_ACCESS_ROOT : 0) |
        (origin & (XR_XIR_CELL_ORIGIN_UNKNOWN | XR_XIR_CELL_ORIGIN_ROOT_UNRESOLVED) ?
            XR_XIR_CELL_ACCESS_UNKNOWN : 0);
    *output = (XrXirCellOriginView){intrinsic,
        f->words ? proof->dependencies+f->dependency_begin+(size_t)value*f->words : NULL,
        f->words,f->scopes ? proof->scope_parameters+f->scope_begin : NULL,f->scopes};
    return XR_XIR_OK;
}
/* The legacy view remains conservative. Only the unique equation replay
 * may substitute every real Class subject and retain actual MODULE checks. */
XR_FUNC XrXirStatus xr_xir_compile_cell_class_origin_view(const XrXirCompileContext *context,
    const XrXirCellProvenance *proof,uint32_t function,uint32_t value,XrXirCellClassOriginView *output) {
    if (!xir_compile_context_valid(context) || !proof || proof->resources!=context->resources ||
        !output || function>=proof->function_count || value>=proof->functions[function].count ||
        output->cells.intrinsic_mask || output->cells.dependencies || output->cells.word_count ||
        output->cells.parameters || output->cells.parameter_count || output->residual_intrinsic_mask ||
        output->dependencies || output->word_count || output->roots || output->root_count)
        return XR_XIR_BAD_STRUCTURE;
    const CellProofFunction *f=&proof->functions[function];
    if (!xir_compile_work(context,10)) return XR_XIR_BUDGET;
    if (f->class_words!=f->class_count/64u+(f->class_count%64u!=0) ||
        f->class_begin>proof->class_count || f->class_count>proof->class_count-f->class_begin ||
        f->class_dependency_begin>proof->class_dependency_count ||
        (f->class_words && f->stored>(proof->class_dependency_count-f->class_dependency_begin)/f->class_words) ||
        (!!proof->class_roots!=!!proof->class_count) ||
        (!!proof->class_dependencies!=!!proof->class_dependency_count) ||
        (f->class_count && f->stored!=f->count)) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t r=0;r<f->class_count;++r) {
        if (!xir_compile_work(context,5)) return XR_XIR_BUDGET;
        XrXirCellClassRoot root=proof->class_roots[f->class_begin+r];
        if (root.place<f->parameters || root.place>=f->count || root.value>=f->count ||
            !(proof->tracked[f->begin+root.value]&4u) ||
            (r && proof->class_roots[f->class_begin+r-1].place>=root.place))
            return XR_XIR_BAD_STRUCTURE;
    }
    const uint64_t *row=f->class_words ? proof->class_dependencies+f->class_dependency_begin+
        (size_t)value*f->class_words : NULL;
    bool present=false;
    for (uint32_t w=0;w<f->class_words;++w) {
        if (!xir_compile_work(context,2)) return XR_XIR_BUDGET;
        if (w+1==f->class_words && f->class_count%64u &&
            row[w]>>(f->class_count%64u)) return XR_XIR_BAD_STRUCTURE;
        present|=row[w]!=0;
    }
    uint8_t origin=f->stored ? proof->origins[f->begin+value] : 0;
    if (!!(origin&XR_XIR_CELL_ORIGIN_ROOT_UNRESOLVED)!=present) return XR_XIR_BAD_STRUCTURE;
    XrXirCellClassOriginView view={0};
    XrXirStatus status=xr_xir_compile_cell_origin_view(context,proof,function,value,&view.cells);
    if (status!=XR_XIR_OK) return status;
    view.residual_intrinsic_mask=(origin&XR_XIR_CELL_ORIGIN_MODULE ? XR_XIR_CELL_ACCESS_ROOT:0) |
        (origin&XR_XIR_CELL_ORIGIN_UNKNOWN ? XR_XIR_CELL_ACCESS_UNKNOWN:0);
    view.dependencies=row;view.word_count=f->class_words;
    view.roots=f->class_count ? proof->class_roots+f->class_begin : NULL;view.root_count=f->class_count;
    if (!xir_compile_work(context,sizeof(view))) return XR_XIR_BUDGET;
    *output=view;return XR_XIR_OK;
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
            for (uint32_t p = 0; p < function->parameters; ++p)
                roles[cursor++] = (uint8_t)xr_xir_cell_provenance_role(proof,f,p);
        }
        offsets[proof->function_count] = cursor;
    }
    xr_xir_compile_cell_provenance_free(proof);
    if (diagnostic) diagnostic->status = status;
    return status;
}
