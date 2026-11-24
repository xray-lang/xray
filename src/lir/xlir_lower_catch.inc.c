/*
 * xray - Lightweight typed scripting with native concurrency
 * https://xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlir_lower_catch.inc.c - Carry a single exact error through ordinary catch
 */

static bool lower_is_plain_error_enum(const Lower *, XrXirType);

static XrXirType lower_catch_type(const Lower *c, const XrXirFunction *f, uint32_t value) {
    const LowerCatchPlan *plan = lower_catch_plan(c, f);
    if (!plan || value < f->parameter_count || value - f->parameter_count >= f->instruction_count)
        return XR_XIR_UNIT;
    return plan->types[value - f->parameter_count];
}

static XrXirType lower_error_operand(const Lower *c, const XrXirFunction *f, uint32_t value) {
    XrXirType type = xr_xir_operand_type(f, value);
    if (type != XR_XIR_ERROR) return type;
    XrXirType caught = lower_catch_type(c, f, value);
    return caught ? caught : type;
}

static uint32_t lower_invoke_failure_predecessor(const Lower *c, const XrXirFunction *f, uint32_t block) {
    uint32_t optional = lower_optional_error_predecessor(c, block);
    const LowerCatchPlan *plan = lower_catch_plan(c, f);
    return optional || !plan ? optional : plan->predecessors[block];
}

static bool lower_catch_reachable(Lower *c,const XrXirFunction *f,uint8_t *active) {
    static const uint8_t edges[] = {0,
#define XR_XIR_OP(name, stages, rule, operands, edges, terminal) edges,
#include "../xir/xxir_ops.def"
#undef XR_XIR_OP
    };
    uint32_t *queue=lower_alloc(c,f->block_count,sizeof(*queue));
    if (!queue) return false;
    active[0]=1;
    queue[0]=0;
    bool good=true;
    for (uint32_t read=0,write=1; good && read<write; ++read) {
        uint32_t block=queue[read],panic=f->blocks[block].panic;
        const XrXirInstruction *term=lower_term(c,f,block);
        if (!term) { good=false; break; }
        uint32_t ordinary=term->op==XR_XIR_INVOKE ? lower_edge_count(c,term) : edges[term->op];
        for (uint32_t e=0; e<ordinary+(panic!=0); ++e) {
            uint32_t next=e<ordinary ? term->targets[e] : panic;
            if (!active[next]) {
                active[next]=1;
                queue[write++]=next;
            }
        }
    }
    xr_free(queue);
    return good;
}

static const XrXirInstruction *lower_catch_predecessor(const XrXirFunction *f,
    uint32_t block,const uint32_t *input) {
    const XrXirBlock *pred=&f->blocks[input[0]];
    const XrXirInstruction *jump=&f->instructions[pred->first+pred->count-1];
    if (pred->panic || jump->targets[0]!=block) return NULL;
    XrXirOp exit = pred->frontier ? XR_XIR_CLEANUP_ERROR : XR_XIR_JUMP;
    return jump->op==exit ? jump : NULL;
}

static XrXirType lower_catch_local_throw(const XrXirFunction *f,const XrXirInstruction *exit,
    uint32_t value,const uint8_t *uses) {
    const XrXirInstruction *erase=lower_place_definition(f,value);
    if (exit->immediate || exit->args[0]!=value || !erase || erase->op!=XR_XIR_ERROR_ERASE ||
        erase+1!=exit || uses[erase-f->instructions]!=2) return XR_XIR_UNIT;
    const XrXirInstruction *error=lower_place_definition(f,erase->args[0]);
    if (!error || error->op!=XR_XIR_ENUM_NEW || error+1!=erase ||
        uses[error-f->instructions]!=1) return XR_XIR_UNIT;
    return error->type;
}

static XrXirType lower_catch_input(const Lower *c,const XrXirFunction *f,uint32_t block,
    const uint32_t *input,const uint8_t *uses) {
    const XrXirInstruction *jump=lower_catch_predecessor(f,block,input);
    if (!jump) return XR_XIR_UNIT;
    const XrXirInstruction *call=lower_forwarded_error_call(f,input[0],input[1],block,jump->op);
    if (call) {
        /* The terminator and handler PHI are the only uses of the released error edge. */
        if (jump->op==XR_XIR_CLEANUP_ERROR && uses[f->blocks[input[0]].first]!=2)
            return XR_XIR_UNIT;
        if (call->immediate<0 || (uint64_t)call->immediate>=c->source->function_count) return XR_XIR_UNIT;
        return c->source->functions[call->immediate].error_type;
    }
    if (jump->op==XR_XIR_CLEANUP_ERROR)
        return lower_catch_local_throw(f,jump,input[1],uses);
    const XrXirInstruction *producer=lower_place_definition(f,input[1]);
    if (!producer || producer->type!=XR_XIR_ERROR) return XR_XIR_UNIT;
    uint32_t index=(uint32_t)(producer-f->instructions);
    if (producer->op==XR_XIR_ERROR_ERASE && producer+1==jump && uses[index]==1)
        return xr_xir_operand_type(f,producer->args[0]);
    if (producer->op==XR_XIR_PHI && index<f->blocks[block].first)
        return lower_catch_type(c,f,input[1]);
    return XR_XIR_UNIT;
}

/* Closed Checked verifies exact incoming pairs and dominance on the edge.
 * A nested handler forwards its earlier, already proved PHI. Never-only
 * landings are absent from the executable graph before any selector folds. */
static XrXirType lower_catch_join_type(const Lower *c,const XrXirFunction *f,uint32_t block,
    const uint8_t *active,const uint8_t *uses) {
    const XrXirBlock *join = &f->blocks[block];
    const XrXirInstruction *phi = &f->instructions[join->first];
    if (join->panic || join->frontier || !phi->args[1]) return XR_XIR_UNIT;
    XrXirType exact = XR_XIR_NEVER;
    for (uint32_t a = 0; a < phi->args[1]; a += 2) {
        const uint32_t *input = &f->operands[phi->args[0] + a];
        if (!active[input[0]]) continue;
        XrXirType error=lower_catch_input(c,f,block,input,uses);
        if (lower_empty_error(error)) return XR_XIR_UNIT;
        if (exact != XR_XIR_NEVER && exact != error) return XR_XIR_UNIT;
        exact = error;
    }
    return lower_is_plain_error_enum(c,exact) ? exact : XR_XIR_UNIT;
}

static void lower_catch_mark_join(LowerCatchPlan *plan,const XrXirFunction *f,uint32_t first,
    const uint8_t *active,XrXirType exact) {
    const XrXirInstruction *phi=&f->instructions[first];
    plan->types[first]=exact;
    for (uint32_t a=0; a<phi->args[1]; a+=2) {
        const uint32_t *input=&f->operands[phi->args[0]+a];
        if (!active[input[0]]) continue;
        const XrXirInstruction *producer=lower_place_definition(f,input[1]);
        if (producer && producer->op==XR_XIR_ERROR_ERASE) {
            uint32_t erased=(uint32_t)(producer-f->instructions);
            plan->types[erased]=exact;
            /* An active local throw proved the immediately preceding constructor. */
            if (f->blocks[input[0]].frontier && erased && f->instructions[erased-1].op==XR_XIR_ENUM_NEW)
                plan->types[erased-1]=exact;
        }
    }
}

static bool lower_catch_local_cleanup_exit(const Lower *c,const XrXirFunction *f,uint32_t block,
    const XrXirInstruction *exit) {
    const XrXirBlock *landing=&f->blocks[block];
    if (!landing->frontier || landing->count<3 || exit->immediate) return false;
    uint32_t erased=f->parameter_count+(uint32_t)(exit-f->instructions)-1;
    return exit->args[0]==erased && exit[-1].op==XR_XIR_ERROR_ERASE && lower_catch_type(c,f,erased)!=XR_XIR_UNIT;
}

/* The canonical pattern compares the tag of a proved narrowing with its
 * ordinal. A singleton enum has tag zero; payload comparisons remain live. */
static bool lower_catch_single_tag(const Lower *c,const XrXirFunction *f,
    const XrXirInstruction *test,bool *selected) {
    if (test->op!=XR_XIR_EQ_INT || test->type!=XR_XIR_BOOL) return false;
    const XrXirInstruction *tag=lower_place_definition(f,test->args[0]);
    const XrXirInstruction *ordinal=lower_place_definition(f,test->args[1]);
    if (!tag || tag->op!=XR_XIR_ENUM_TAG || tag->type!=XR_XIR_I64 ||
        !ordinal || ordinal->op!=XR_XIR_CONST_INT || ordinal->type!=XR_XIR_I64) return false;
    const XrXirInstruction *narrow=lower_place_definition(f,tag->args[0]);
    if (!narrow || narrow->op!=XR_XIR_ERROR_NARROW ||
        lower_catch_type(c,f,narrow->args[0])!=narrow->type) return false;
    XrXirTypeDescription description={0};
    const XrXirCompileContext *context=xr_xir_compile_artifact_context(c->request->closed);
    if (xr_xir_compile_type_description(context,c->source->types,narrow->type,&description)!=XR_XIR_OK ||
        description.nominal_kind!=XR_XIR_NOMINAL_ENUM || description.variant_count!=1) return false;
    *selected=ordinal->immediate==0;
    return true;
}

static bool lower_catch_selector(const Lower *c,const XrXirFunction *f,
    const XrXirInstruction *test,bool *selected) {
    if (!test) return false;
    if (test->op!=XR_XIR_ERROR_IS) return lower_catch_single_tag(c,f,test,selected);
    XrXirType exact=lower_catch_type(c,f,test->args[0]);
    if (!exact) return false;
    *selected=exact==(XrXirType)test->immediate;
    return true;
}

static bool lower_catch_errors(Lower *c, const XrXirFunction *f) {
    if (lower_catch_plan(c, f)) return true;
    LowerCatchPlan *plan = NULL;
    uint8_t *active=NULL,*uses=NULL;
    bool good=true;
    for (uint32_t b=0; good && b<f->block_count; ++b) {
        uint32_t first = f->blocks[b].first;
        const XrXirInstruction *phi = &f->instructions[first];
        if (phi->op != XR_XIR_PHI || phi->type != XR_XIR_ERROR || lower_optional_error_join(c, b))
            continue;
        if (!uses) {
            active=lower_alloc(c,f->block_count,1);
            uses=lower_alloc(c,f->instruction_count,1);
            good=active && uses && lower_catch_reachable(c,f,active);
            if (!good) break;
            lower_count_operand_uses(c,f,uses);
        }
        if (!active[b]) continue;
        XrXirType exact=lower_catch_join_type(c,f,b,active,uses);
        if (!exact) continue;
        if (!plan) {
            if (!c->catch_errors)
                c->catch_errors = lower_alloc(c, c->source->function_count, sizeof(*c->catch_errors));
            if (!c->catch_errors) { good=false; break; }
            plan = &c->catch_errors[c->function];
            plan->types = lower_alloc(c, f->instruction_count, sizeof(*plan->types));
            plan->branches = lower_alloc(c, f->block_count, sizeof(*plan->branches));
            plan->predecessors = lower_alloc(c, f->block_count, sizeof(*plan->predecessors));
            if (!plan->types || !plan->branches || !plan->predecessors) { good=false; break; }
        }
        lower_catch_mark_join(plan,f,first,active,exact);
    }
    xr_free(active);
    xr_free(uses);
    if (!good || !plan) return good;
    for (uint32_t b = 0; b < f->block_count; ++b) {
        const XrXirInstruction *term = lower_term(c, f, b);
        if (!term) return false;
        if (term->op == XR_XIR_INVOKE) {
            uint32_t failed = term->targets[1];
            const XrXirInstruction *exit = lower_term(c, f, failed);
            if (!exit) return false;
            if ((exit->op==XR_XIR_JUMP || exit->op==XR_XIR_CLEANUP_ERROR) &&
                plan->types[f->blocks[exit->targets[0]].first] &&
                lower_forwarded_error_call(f,failed,f->parameter_count+f->blocks[failed].first,
                    exit->targets[0],exit->op)==term)
                plan->predecessors[failed] = b + 1;
        } else if (term->op == XR_XIR_BRANCH) {
            const XrXirInstruction *test = lower_place_definition(f, term->args[0]);
            bool selected=false;
            if (!lower_catch_selector(c,f,test,&selected)) continue;
            uint32_t target=term->targets[selected ? 0 : 1];
            plan->branches[b] = (XrXirInstruction){.op = XR_XIR_JUMP, .type = XR_XIR_UNIT,
                .targets = {target}};
        }
    }
    return true;
}

static void lower_free_error_plans(Lower *c) {
    for (uint32_t f = 0; f < c->source->function_count && (c->optional_errors || c->catch_errors); ++f) {
        if (c->optional_errors) xr_free(c->optional_errors[f]);
        if (c->catch_errors) {
            xr_free(c->catch_errors[f].types);
            xr_free(c->catch_errors[f].branches);
            xr_free(c->catch_errors[f].predecessors);
        }
    }
    xr_free(c->optional_errors);
    xr_free(c->catch_errors);
}

static bool lower_catch_value(LowerFunction *f, uint32_t block, const XrXirInstruction *op,
    XrLirValue *result) {
    if (op->op != XR_XIR_ERROR_IS) return lower_value(f, op->args[0], result);
    Lower *c = f->owner;
    XrXirType exact = lower_catch_type(c, f->source, op->args[0]);
    XrLirInstruction select = {.op = XR_LIR_CONST, .type = XR_LIR_I1,
        .bits = exact == (XrXirType)op->immediate, .source = lower_source(c)};
    return lower_build(c, xr_lir_emit_value(c->module, f->blocks[block], &select, result));
}
