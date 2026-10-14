/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_invocation_solution.inc.c - Exact closed equations and staged canonical formulas
 *
 * KEY CONCEPT:
 *   A staged result owns its canonical records. No borrowed producer, pending
 *   equation or merely empty lattice state can become an executable permission.
 */
/* This is the final receiving owner. Each term vector is written once and
 * transfers to its public contract only after independent comparison. */
static void effect_invocation_result_clear(EffectFinalFunctions *result) {
    if (!result) return;
    for (uint32_t f=0;f<result->count;++f)
        xr_compile_resources_free(result->functions[f].storage);
    xr_compile_resources_free(result->functions);
    xr_compile_resources_free(result->facts);xr_compile_resources_free(result->witnesses);
}

static void effect_invocation_result_free(EffectFinalFunctions *result) {
    if (!result) return;
    effect_invocation_result_clear(result);xr_compile_resources_free(result);
}

static bool effect_invocation_frontier(XrXirOp op) {
    return op==XR_XIR_CALL || op==XR_XIR_INVOKE || op==XR_XIR_CALL_DEFAULT ||
        op==XR_XIR_INVOKE_DEFAULT || op==XR_XIR_CALL_REQUIREMENT ||
        op==XR_XIR_CLEANUP_REGISTER || op==XR_XIR_CALL_INDIRECT || op==XR_XIR_INVOKE_INDIRECT;
}

/* Compare the exact union of local contributions and executing children.
 * An inflated OR result is rejected, including an unrooted recursive fact.
 * Certificate forests separately prove a genuine local terminal for every bit. */
static XrXirStatus effect_invocation_closed(EffectInvocationWorkspace *owner) {
    if (!owner || !effect_body_material_shape(owner->final.material) || !owner->graph || owner->pending || !owner->final.count)
        return XR_XIR_BAD_STRUCTURE;
    uint64_t *expected=NULL;size_t capacity=0;
    XrXirStatus status=XR_XIR_OK;uint64_t linked=0;
    for (uint32_t n=0;n<owner->final.count && status==XR_XIR_OK;++n) {
        const EffectInvocationNode *node=&owner->final.nodes[n];
        if (!node->expanded || node->queued || node->body>=owner->body.domain.function_count) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        EffectInvocationBasis basis={0};status=effect_invocation_basis(owner,node->root,&basis);
        if (status!=XR_XIR_OK) break;
        uint32_t dependencies=(uint32_t)(((uint64_t)basis.parameters+63)/64);
        EffectBodyFunctionDimensions head={0};status=effect_invocation_dimensions(owner,node->body,&head);
        if (status!=XR_XIR_OK) break;
        uint32_t instructions=head.instruction_count;
        uint32_t contexts=(uint32_t)(((uint64_t)instructions+63)/64);
        uint64_t words=(uint64_t)dependencies*2+contexts;
        if (!xir_compile_work(owner->work,9)) { status=XR_XIR_BUDGET;break; }
        if (!effect_invocation_node_shape(node,basis.parameters,instructions)) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        if (words>SIZE_MAX/sizeof(*expected) || (words && (!node->cells || !node->local_cells))) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        if (words>capacity) {
            uint64_t *replacement=xir_compile_alloc(owner->work,(size_t)words*sizeof(*expected),&status);
            if (!replacement) break;
            xr_compile_resources_free(expected);expected=replacement;capacity=(size_t)words;
        }
        if (!xir_compile_work(owner->work,words*sizeof(*expected)+3)) { status=XR_XIR_BUDGET;break; }
        if (words) memcpy(expected,node->local_cells,(size_t)words*sizeof(*expected));
        uint32_t mask=node->local_mask;bool deferred=node->local_deferred;
        const uint64_t *parameter_bits=effect_invocation_node_row(node,EFFECT_INVOCATION_PARAMETERS,false);
        const uint64_t *context_bits=effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,false);
        for (uint32_t e=node->edge_head;e!=UINT32_MAX;e=owner->final.edges[e].next) {
            if (!xir_compile_work(owner->work,6)) { status=XR_XIR_BUDGET;break; }
            if (e>=owner->final.edge_count) { status=XR_XIR_BAD_STRUCTURE;break; }
            EffectInvocationEdge edge=owner->final.edges[e];++linked;
            if (linked>owner->final.edge_count || edge.caller!=n || edge.target>=owner->final.count ||
                edge.instruction>=instructions || (edge.next!=UINT32_MAX && edge.next>=e) ||
                owner->final.nodes[edge.target].root!=node->root) { status=XR_XIR_BAD_STRUCTURE;break; }
            if (edge.latent) continue;
            const EffectInvocationNode *child=&owner->final.nodes[edge.target];
            if (!xir_compile_work(owner->work,9)) { status=XR_XIR_BUDGET;break; }
            EffectBodyFunctionDimensions child_head={0};
            status=effect_invocation_dimensions(owner,child->body,&child_head);
            if (status!=XR_XIR_OK) break;
            if (child->body>=owner->body.domain.function_count ||
                !effect_invocation_node_shape(child,basis.parameters,child_head.instruction_count)) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
            mask|=child->intrinsic_mask;deferred|=child->deferred;
            const uint64_t *child_parameters=effect_invocation_node_row(child,EFFECT_INVOCATION_PARAMETERS,false);
            for (uint32_t w=0;w<dependencies;++w) {
                if (!xir_compile_work(owner->work,4)) { status=XR_XIR_BUDGET;break; }
                expected[w]|=child->cells[w];expected[dependencies+w]|=child_parameters[w];
            }
            if (status!=XR_XIR_OK) break;
            if (child->deferred) expected[(size_t)dependencies*2+edge.instruction/64]|=
                UINT64_C(1)<<(edge.instruction%64);
        }
        if (status!=XR_XIR_OK) break;
        if (mask!=node->intrinsic_mask || deferred!=node->deferred ||
            (mask&~(XR_XIR_CALLABLE_ROOT_REQUIRED|XR_XIR_CALLABLE_ROOT_UNRESOLVED))) {
            status=XR_XIR_BAD_STRUCTURE;break;
        }
        if (basis.parameters%64) {
            uint64_t valid=(UINT64_C(1)<<(basis.parameters%64))-1;
            if ((node->cells[dependencies-1]|parameter_bits[dependencies-1])&~valid) {
                status=XR_XIR_BAD_STRUCTURE;break;
            }
        }
        if (instructions%64 && (context_bits[contexts-1]&
            ~((UINT64_C(1)<<(instructions%64))-1))) { status=XR_XIR_BAD_STRUCTURE;break; }
        bool any=false;
        for (size_t w=0;w<(size_t)words;++w) {
            if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
            if (expected[w]!=node->cells[w]) { status=XR_XIR_BAD_STRUCTURE;break; }
        }
        for (uint32_t w=0;w<contexts && status==XR_XIR_OK;++w) {
            if (!xir_compile_work(owner->work,2)) { status=XR_XIR_BUDGET;break; }
            uint64_t remaining=context_bits[w];
            while (remaining && status==XR_XIR_OK) {
                if (!xir_compile_work(owner->work,3)) { status=XR_XIR_BUDGET;break; }
                uint32_t i=w*64+(uint32_t)__builtin_ctzll(remaining);
                remaining&=remaining-1;any=true;
                EffectBodyInstructionShape shape={0};status=effect_invocation_shape(owner,node->body,i,&shape);
                if (status==XR_XIR_OK && !effect_invocation_frontier(shape.op)) status=XR_XIR_BAD_STRUCTURE;
            }
        }
        if (status==XR_XIR_OK && any!=deferred) status=XR_XIR_BAD_STRUCTURE;
    }
    xr_compile_resources_free(expected);
    if (status==XR_XIR_OK && linked!=owner->final.edge_count) status=XR_XIR_BAD_STRUCTURE;
    return status;
}

/* The same canonical enumerator counts, writes or compares a bounded
 * receiving span. Comparison reads the actual stored kind/index directly;
 * no expected array or permission result survives this one request. */
/* Sources are lifetime adapters for actual role metadata, not permission
 * hints. The immutable role inventory is explicit when available;
 * otherwise the generating owner supplies its actual contracts. */
typedef struct EffectInvocationTermSource {
    const XrXirCompileContext *work;
    const EffectInvocationFinal *equations;
    const uint8_t *const *kinds;
    const XrXirEffects *effects;
} EffectInvocationTermSource;
typedef struct EffectInvocationTermTarget {
    uint32_t limit,kind,index;
    bool *present;
} EffectInvocationTermTarget;

static XrXirStatus effect_body_result_head(const XrXirCompileContext *work,
    const EffectBodyMaterial *material,uint32_t function) {
    if (!xir_compile_context_valid(work) || !effect_body_material_shape(material) ||
        material->resources!=work->resources || function>=effect_body_count(material)) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyFunction *head=&material->functions[function];
    if ((head->name_length && !head->name) || (head->parameter_count && !head->parameters) ||
        (head->block_count && !head->blocks) || (head->operand_count && !head->operands) ||
        (head->instruction_count && (!head->runs || !head->run_count)) ||
        head->run_count>head->run_capacity) return XR_XIR_BAD_STRUCTURE;
    return xir_compile_work(work,11)?XR_XIR_OK:XR_XIR_BUDGET;
}

static uint32_t effect_invocation_result_parameter(const EffectInvocationTermSource *source,
    uint32_t function,uint32_t parameter) {
    if (source->kinds)
        return source->kinds[function]?source->kinds[function][parameter]:UINT32_MAX;
    if (source->effects) {
        const XrXirFunctionEffectContract *contract=&source->effects->contracts[function];
        if (contract->parameter_count!=source->equations->material->functions[function].parameter_count ||
            !contract->parameters) return UINT32_MAX;
        return contract->parameters[parameter].kind;
    }
    return UINT32_MAX;
}

static XrXirStatus effect_invocation_result_terms(const EffectInvocationTermSource *source,
    uint32_t function,XrXirRootTerm *terms,const XrXirRootTerm *expected,
    const EffectInvocationTermTarget *target,uint32_t *count) {
    if (!source || !source->equations || !xir_compile_context_valid(source->work) ||
        !effect_body_material_shape(source->equations->material) || source->equations->material->resources!=source->work->resources ||
        !source->equations->nodes ||
        (!source->effects && !source->kinds) ||
        (!source->kinds && source->effects && !source->effects->contracts) || !target || !count || (terms && expected) || !source->equations->roots ||
        function>=source->equations->material->domain.function_count || source->equations->roots[function]>=source->equations->count)
        return XR_XIR_BAD_STRUCTURE;
    const EffectInvocationNode *node=&source->equations->nodes[source->equations->roots[function]];
    const EffectBodyFunction *body=&source->equations->material->functions[function];
    XrXirStatus read_status=effect_body_result_head(source->work,source->equations->material,function);
    if (read_status!=XR_XIR_OK) return read_status;
    if (!source->equations->root_spaces || node->root!=source->equations->root_spaces[function] ||
        node->body!=function || node->parameter_count!=body->parameter_count)
        return XR_XIR_BAD_STRUCTURE;
    if (node->root>=effect_body_count(source->equations->material) || !node->expanded || node->queued)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t root_parameters=source->equations->material->functions[node->root].parameter_count;
    if (!xir_compile_work(source->work,9)) return XR_XIR_BUDGET;
    if (!effect_invocation_node_shape(node,root_parameters,body->instruction_count)) return XR_XIR_BAD_STRUCTURE;
    bool shared=node->root!=function;
    if (shared) for (uint32_t p=0;p<body->parameter_count;++p) {
        if (!xir_compile_work(source->work,3)) return XR_XIR_BUDGET;
        if (xr_xir_callable_signature(source->equations->material->domain.types,body->parameters[p]) ||
            xr_xir_type_is_cell(source->equations->material->domain.types,body->parameters[p])) return XR_XIR_BAD_STRUCTURE;
    }
    if (shared) {
        XrXirStatus status=effect_body_result_head(source->work,source->equations->material,node->root);
        if (status!=XR_XIR_OK) return status;
        uint32_t words=(uint32_t)(((uint64_t)source->equations->material->functions[node->root].parameter_count+63)/64);
        for (uint32_t w=0;w<words;++w) {
            if (!xir_compile_work(source->work,3)) return XR_XIR_BUDGET;
            if (node->cells[w] || effect_invocation_node_row(node,EFFECT_INVOCATION_PARAMETERS,false)[w]) return XR_XIR_BAD_STRUCTURE;
        }
    }
    uint64_t at=0;bool found=false;uint32_t limit=target->limit;
    for (uint32_t kind=XR_XIR_ROOT_TERM_PARAMETER;kind<=XR_XIR_ROOT_TERM_CELL_PARAMETER;++kind) {
        uint32_t items=kind==XR_XIR_ROOT_TERM_CONTEXT_CALL?body->instruction_count:shared?0:body->parameter_count;
        const uint64_t *bits=kind==XR_XIR_ROOT_TERM_PARAMETER?effect_invocation_node_row(node,EFFECT_INVOCATION_PARAMETERS,false):
            kind==XR_XIR_ROOT_TERM_CELL_PARAMETER?node->cells:effect_invocation_node_row(node,EFFECT_INVOCATION_CONTEXTS,false);
        if (!xir_compile_work(source->work,1)) return XR_XIR_BUDGET;
        uint32_t words=(uint32_t)(((uint64_t)items+63)/64);
        if (words && !bits) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t w=0;w<words;++w) {
            if (!xir_compile_work(source->work,3)) return XR_XIR_BUDGET;
            uint64_t remaining=bits[w];
            if (w+1==words && items%64 && (remaining&~((UINT64_C(1)<<(items%64))-1)))
                return XR_XIR_BAD_STRUCTURE;
            /* Ascending words and lowest bits preserve canonical term order. */
            while (remaining) {
                if (!xir_compile_work(source->work,4)) return XR_XIR_BUDGET;
                uint32_t i=w*64+(uint32_t)__builtin_ctzll(remaining);
                remaining&=remaining-1;
                if (kind==XR_XIR_ROOT_TERM_PARAMETER &&
                    (!xr_xir_callable_signature(source->equations->material->domain.types,body->parameters[i]) ||
                    effect_invocation_result_parameter(source,function,i)!=XR_XIR_EFFECT_PARAMETER_VARIABLE))
                    return XR_XIR_BAD_STRUCTURE;
                if (kind==XR_XIR_ROOT_TERM_CELL_PARAMETER && !xr_xir_type_is_cell(source->equations->material->domain.types,body->parameters[i]))
                    return XR_XIR_BAD_STRUCTURE;
                if (kind==XR_XIR_ROOT_TERM_CONTEXT_CALL) {
                    EffectBodyRead read=effect_body_compile_read(source->work);XrXirOp opcode=XR_XIR_INVALID;
                    read_status=effect_body_instruction_op(&read,body,i,&opcode);
                    if (read_status!=XR_XIR_OK) return read_status;
                    if (!effect_invocation_frontier(opcode)) return XR_XIR_BAD_STRUCTURE;
                }
                if (at>=UINT32_MAX) return XR_XIR_BUDGET;
                if (limit!=UINT32_MAX) {
                    if (!xir_compile_work(source->work,1)) return XR_XIR_BUDGET;
                    if (at>=limit) return XR_XIR_BAD_STRUCTURE;
                }
                if (terms) {
                    if (!xir_compile_work(source->work,sizeof(*terms))) return XR_XIR_BUDGET;
                    terms[at]=(XrXirRootTerm){kind,i};
                }
                if (expected) {
                    if (!xir_compile_work(source->work,4)) return XR_XIR_BUDGET;
                    if (expected[at].kind!=kind || expected[at].index!=i) return XR_XIR_BAD_STRUCTURE;
                }
                if (target->present && kind==target->kind && i==target->index) found=true;
                ++at;
            }
        }
    }
    if (target->present) {
        if (!xir_compile_work(source->work,sizeof(*target->present))) return XR_XIR_BUDGET;
        *target->present=found;
    }
    *count=(uint32_t)at;return XR_XIR_OK;
}
static inline XrXirStatus effect_invocation_terms_read(EffectInvocationWorkspace *owner,
    uint32_t function,XrXirRootTerm *terms,const XrXirRootTerm *expected,
    uint32_t limit,uint32_t *count) {
    if (!owner || !xir_compile_context_valid(owner->work) || !owner->effects ||
        !owner->effects->contracts) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(owner->work,sizeof(EffectInvocationTermSource)+sizeof(EffectInvocationTermTarget)))
        return XR_XIR_BUDGET;
    const EffectInvocationTermSource source={owner->work,&owner->final,NULL,owner->effects};
    const EffectInvocationTermTarget target={limit,0,0,NULL};
    return effect_invocation_result_terms(&source,function,terms,expected,&target,count);
}


static inline XrXirStatus effect_invocation_terms(EffectInvocationWorkspace *owner,
    uint32_t function,XrXirRootTerm *terms,uint32_t *count) {
    return effect_invocation_terms_read(owner,function,terms,NULL,UINT32_MAX,count);
}

/* The same constructor owns the final term spans for generating equations or
 * for an independent replay of an immutable certificate. Neither adapter
 * retains its caller, and no second canonical formula inventory is stored. */
static XrXirStatus effect_invocation_result_new(const EffectInvocationTermSource *source,
    EffectFinalFunctions **output) {
    if (!source || !source->equations || !xir_compile_context_valid(source->work) ||
        !output || *output || !effect_body_material_shape(source->equations->material) ||
        source->equations->material->resources!=source->work->resources)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t count=effect_body_count(source->equations->material);
    if (!count || !source->equations->roots || !source->equations->nodes) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=XR_XIR_OK;
    EffectFinalFunctions *result=xir_compile_calloc(source->work,1,sizeof(*result),&status);
    if (!result) return status==XR_XIR_OK?XR_XIR_OUT_OF_MEMORY:status;
    if (!xir_compile_work(source->work,sizeof(result->resources)+sizeof(result->capacity))) {
        effect_invocation_result_free(result);return XR_XIR_BUDGET;
    }
    result->resources=source->work->resources;result->capacity=count;
    if ((uint64_t)count*sizeof(*result->functions)>SIZE_MAX) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) result->functions=xir_compile_alloc(source->work,
        (size_t)count*sizeof(*result->functions),&status);
    if (!result->functions) status=status==XR_XIR_OK?XR_XIR_OUT_OF_MEMORY:status;
    uint64_t total=0;
    for (uint32_t f=0;f<count && status==XR_XIR_OK;++f) {
        if (!xir_compile_work(source->work,sizeof(*result->functions)+sizeof(result->count))) { status=XR_XIR_BUDGET;break; }
        result->functions[f]=(EffectFinalFunction){0};++result->count;
        EffectFinalFunction *record=&result->functions[f];uint32_t terms=0;
        if (!xir_compile_work(source->work,sizeof(EffectInvocationTermTarget))) { status=XR_XIR_BUDGET;break; }
        const EffectInvocationTermTarget unlimited={UINT32_MAX,0,0,NULL};
        status=effect_invocation_result_terms(source,f,NULL,NULL,&unlimited,&terms);total+=terms;
        if (status!=XR_XIR_OK) break;
        if (total>UINT32_MAX || (uint64_t)terms*sizeof(*record->storage)>SIZE_MAX) {
            status=XR_XIR_BUDGET;break;
        }
        XrXirRootTerm *storage=terms?xir_compile_alloc(source->work,
            (size_t)terms*sizeof(*storage),&status):NULL;
        if (terms && !storage) { if (status==XR_XIR_OK) status=XR_XIR_OUT_OF_MEMORY;break; }
        if (!xir_compile_work(source->work,sizeof(record->storage)+sizeof(record->capacity))) {
            xr_compile_resources_free(storage);status=XR_XIR_BUDGET;break;
        }
        record->storage=storage;record->capacity=terms;
        if (!xir_compile_work(source->work,sizeof(EffectInvocationTermTarget))) { status=XR_XIR_BUDGET;break; }
        const EffectInvocationTermTarget bounded={terms,0,0,NULL};uint32_t written=0;
        status=effect_invocation_result_terms(source,f,storage,NULL,&bounded,&written);
        if (status!=XR_XIR_OK) break;
        if (written!=terms) { status=XR_XIR_BAD_STRUCTURE;break; }
        if (!xir_compile_work(source->work,sizeof(record->formula))) { status=XR_XIR_BUDGET;break; }
        record->formula=(XrXirRootFormula){source->equations->nodes[source->equations->roots[f]].intrinsic_mask,
            terms,storage};
    }
    if (status==XR_XIR_OK && !xir_compile_work(source->work,sizeof(result->term_count))) status=XR_XIR_BUDGET;
    if (status==XR_XIR_OK) result->term_count=(uint32_t)total;
    if (status!=XR_XIR_OK) { effect_invocation_result_free(result);return status; }
    *output=result;return XR_XIR_OK;
}

/* The local relation has one actual canonical result, created directly from
 * this producer's complete input. Stage transfers this owner; seal independently
 * derives the actual input again before comparing any result or permission. */
static XrXirStatus effect_invocation_local_result_new(const XrXirCompileContext *work,
    const EffectInvocationWorkspace *owner,const EffectInvocationLocalInput *inputs,
    EffectFinalFunctions **output) {
    if (!xir_compile_context_valid(work) || !owner || !inputs || !output || *output ||
        !effect_body_material_shape(owner->final.material) || owner->final.material->resources!=work->resources)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t count=effect_body_count(owner->final.material);XrXirStatus status=XR_XIR_OK;
    if (!count || (uint64_t)count>SIZE_MAX/sizeof(EffectFinalFunction)) return XR_XIR_BUDGET;
    EffectFinalFunctions *result=xir_compile_alloc(work,sizeof(*result),&status);
    if (!result) return status;
    if (!xir_compile_work(work,sizeof(*result)+sizeof(result->functions))) {
        xr_compile_resources_free(result);return XR_XIR_BUDGET;
    }
    *result=(EffectFinalFunctions){.resources=work->resources,.capacity=count};
    result->functions=xir_compile_alloc(work,(size_t)count*sizeof(*result->functions),&status);
    if (!result->functions) { effect_invocation_result_free(result);return status; }
    for (uint32_t f=0;f<count;++f) {
        if (!xir_compile_work(work,sizeof(*result->functions)+sizeof(result->count)+2)) {
            effect_invocation_result_free(result);return XR_XIR_BUDGET;
        }
        uint32_t mask=(inputs[f].facts.requires_root?XR_XIR_CALLABLE_ROOT_REQUIRED:0)|
            (inputs[f].facts.unresolved?XR_XIR_CALLABLE_ROOT_UNRESOLVED:0);
        result->functions[f]=(EffectFinalFunction){.formula={.constant_mask=mask}};
        ++result->count;
    }
    if (!xir_compile_work(work,sizeof(*output))) { effect_invocation_result_free(result);return XR_XIR_BUDGET; }
    *output=result;return XR_XIR_OK;
}

/* Staging never changes public contracts. Closure and subsequent independent
 * comparison both consume the complete current equations before transfer. */
static XrXirStatus effect_invocation_stage(EffectInvocationWorkspace *owner,
    EffectFinalFunctions **output) {
    if (!output || *output || !owner || !xir_compile_context_valid(owner->work) ||
        !owner->effects || !owner->effects->contracts) return XR_XIR_BAD_STRUCTURE;
    if (owner->prepared_result) {
        EffectFinalFunctions *result=owner->prepared_result;
        if (!effect_body_material_shape(owner->final.material) || owner->final.material->resources!=owner->work->resources ||
            result->resources!=owner->work->resources || result->count!=effect_body_count(owner->final.material) ||
            result->count!=result->capacity || !result->functions || result->facts || result->witnesses ||
            owner->pending || owner->final.count!=result->count || !owner->final.nodes || !owner->final.roots)
            return XR_XIR_BAD_STRUCTURE;
        for (uint32_t f=0;f<result->count;++f) {
            if (!xir_compile_work(owner->work,8)) return XR_XIR_BUDGET;
            const EffectInvocationNode *node=&owner->final.nodes[f];
            if (owner->final.roots[f]!=f || node->body!=f || node->root || node->parameter_count ||
                !node->expanded || node->queued || !effect_invocation_node_shape(node,0,
                    owner->final.material->functions[f].instruction_count)) return XR_XIR_BAD_STRUCTURE;
        }
        if (!xir_compile_work(owner->work,2*sizeof(void *))) return XR_XIR_BUDGET;
        *output=result;owner->prepared_result=NULL;return XR_XIR_OK;
    }
    XrXirStatus status=effect_invocation_closed(owner);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(owner->work,sizeof(EffectInvocationTermSource))) return XR_XIR_BUDGET;
    const EffectInvocationTermSource source={owner->work,&owner->final,NULL,owner->effects};
    return effect_invocation_result_new(&source,output);
}
