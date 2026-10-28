/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_type_match.inc.c - Bounded equality under explicit type substitution
 *
 * KEY CONCEPT:
 *   Field proofs and call proofs share one traversal of immutable type nodes.
 */
typedef struct TypeMatchTerm { XrXirType type; bool substituted; } TypeMatchTerm;
typedef struct TypeMatchUnion {
    TypeMatchTerm *terms[2];
    uint32_t count[2];
    bool erased[2],subset;
} TypeMatchUnion;
typedef struct TypeMatchFrame {
    const XrXirTypeNode *from, *to;
    const XrXirTypes *source_types;
    const XrXirType *arguments;
    uint32_t argument_count,next,candidate,side;
    TypeMatchUnion *errors;
    bool awaiting;
} TypeMatchFrame;
typedef struct TypeMatchMemory {
    struct TypeMatchMemory *next;
    TypeMatchFrame *frames;
    uint32_t capacity;
    bool claimed;
} TypeMatchMemory;
static void type_match_scratch_free(XrXirTypeMatchScratch *scratch,XirReadScratch *read) {
    if (!scratch) return;
    TypeMatchMemory *memory = scratch->memory;
    while (memory) {
        TypeMatchMemory *next = memory->next;
        xir_read_scratch_release(read,memory);
        memory = next;
    }
    *scratch = (XrXirTypeMatchScratch) {0};
}
XR_FUNC void xr_xir_type_match_scratch_free(XrXirTypeMatchScratch *scratch) {
    type_match_scratch_free(scratch,NULL);
}
static XrXirStatus type_match_stack_claim(const XrXirCompileContext *context,
    XrXirTypeMatchScratch *scratch, uint32_t count, TypeMatchMemory **output,XirReadScratch *read) {
    for (TypeMatchMemory *memory = scratch->memory; memory; memory = memory->next) {

        if (!memory->claimed && memory->capacity >= count) {
            memory->claimed = true;
            *output = memory;
            return XR_XIR_OK;
        }
    }
    uint64_t bytes = sizeof(TypeMatchMemory) + (uint64_t) count * sizeof(TypeMatchFrame);
    if (!count || bytes > SIZE_MAX) return XR_XIR_BUDGET;
    XrXirStatus status = XR_XIR_OK;
    TypeMatchMemory *memory = xir_read_scratch_alloc(context,read,(size_t)bytes,false,&status);
    if (!memory) return status;
    *memory = (TypeMatchMemory) {scratch->memory, (TypeMatchFrame *) (memory + 1), count, true};
    scratch->memory = memory;
    *output = memory;
    return XR_XIR_OK;
}
/* Reserve only the depth actually traversed. Nested substitutions claim
 * separate blocks; no live frame may alias a reused scratch reservation. */
static XrXirStatus type_match_stack_grow(const XrXirCompileContext *context,
    XrXirTypeMatchScratch *scratch, TypeMatchMemory **current, uint32_t capacity,
    uint32_t used,XirReadScratch *read) {
    TypeMatchMemory *next = NULL;
    XrXirStatus status = type_match_stack_claim(context, scratch, capacity, &next,read);
    if (status != XR_XIR_OK) return status;
    uint64_t bytes = (uint64_t)used * sizeof(*next->frames);

    memcpy(next->frames, (*current)->frames, (size_t)bytes);
    memset((*current)->frames,0,(size_t)bytes);
    (*current)->claimed = false; *current = next;
    return XR_XIR_OK;
}

typedef struct TypeMatchContext {
    const XrXirTypes *source_types;
    const XrXirTypes *types;
    const XrXirType *arguments;
    uint32_t count;
    XrXirCompileContext *remaining;
    XrXirTypeMatchScratch *scratch;
    XirReadScratch *read;
    void *owner;
    XrXirNominalMatch nominal_match;
} TypeMatchContext;
static XrXirStatus type_match_run(const XrXirCompileContext *context,
    const XirTypeMatchRead *request,XrXirTypeMatchScratch *scratch);

/* A substituted argument already belongs to the receiving pool. Its binders
 * are never interpreted again under the original declaration's arguments. */
static XrXirStatus type_match_terms(TypeMatchContext *c,XrXirType root,bool expected,
    TypeMatchTerm *terms,uint32_t *count,bool *erased) {
    const XrXirTypes *types=expected ? c->source_types : c->types;
    if (!error_union_shape(types,root)) return XR_XIR_BAD_TYPE;
    const XrXirTypeNode *node=error_union_node(types,root);
    uint32_t n=node ? node->parameter_count : 1;uint64_t total=0;
    for (uint32_t i=0;i<n;++i) {
        XrXirType type=node ? node->parameters[i].type : root;
        uint32_t id=(uint32_t)type;bool substituted=false;
        if (expected && c->count && id>=XR_XIR_TYPE_PARAMETER_BASE && id<XR_XIR_TYPE_PARAMETER_LIMIT) {
            uint32_t p=id-XR_XIR_TYPE_PARAMETER_BASE;
            if (p>=c->count || !c->arguments) return XR_XIR_BAD_TYPE;
            type=c->arguments[p];substituted=true;
        }
        const XrXirTypes *owner=substituted ? c->types : types;
        if (!error_union_shape(owner,type)) return XR_XIR_BAD_TYPE;
        const XrXirTypeNode *expanded=error_union_node(owner,type);
        uint32_t leaves=expanded ? expanded->parameter_count : 1;
        for (uint32_t p=0;p<leaves;++p) {
            XrXirType leaf=expanded ? expanded->parameters[p].type : type;
            if (leaf==XR_XIR_NEVER) continue;
            *erased|=leaf==XR_XIR_ERROR;
            if (total==UINT32_MAX) return XR_XIR_OVERFLOW;
            if (terms) terms[total]=(TypeMatchTerm){leaf,substituted};
            ++total;
        }
    }
    *count=(uint32_t)total;return XR_XIR_OK;
}
static XrXirStatus type_match_union(TypeMatchContext *c,XrXirType expected,
    XrXirType actual,bool subset,TypeMatchFrame *frame) {
    uint32_t counts[2]={0};bool erased[2]={false};
    XrXirStatus status=type_match_terms(c,expected,true,NULL,&counts[0],&erased[0]);
    if (status==XR_XIR_OK) status=type_match_terms(c,actual,false,NULL,&counts[1],&erased[1]);
    if (status!=XR_XIR_OK) return status;
    if (erased[0] || erased[1])
        return (subset ? erased[0] : erased[0]==erased[1]) ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    if (!counts[0] && !counts[1]) return XR_XIR_OK;
    if ((!counts[0] && counts[1]) || (!subset && counts[0] && !counts[1])) return XR_XIR_BAD_TYPE;
    if (subset && !counts[1]) return XR_XIR_OK;
    uint64_t bytes=sizeof(TypeMatchUnion)+((uint64_t)counts[0]+counts[1])*sizeof(TypeMatchTerm);
    if (bytes>SIZE_MAX) return XR_XIR_OVERFLOW;
    TypeMatchUnion *memory=xir_read_scratch_alloc(c->remaining,c->read,(size_t)bytes,true,&status);
    if (!memory) return status;
    *memory=(TypeMatchUnion){.terms={(TypeMatchTerm *)(memory+1),(TypeMatchTerm *)(memory+1)+counts[0]},
        .count={counts[0],counts[1]},.subset=subset};
    status=type_match_terms(c,expected,true,memory->terms[0],&memory->count[0],&memory->erased[0]);
    if (status==XR_XIR_OK) status=type_match_terms(c,actual,false,memory->terms[1],&memory->count[1],&memory->erased[1]);
    if (status!=XR_XIR_OK) {xir_read_scratch_release(c->read,memory);return status;}
    *frame=(TypeMatchFrame){.source_types=c->source_types,.arguments=c->arguments,.argument_count=c->count,
        .side=subset?1u:0u,.errors=memory};
    return XR_XIR_OK;
}
static XrXirStatus type_match_pair(TypeMatchContext *c,XrXirType expected,
    XrXirType actual,TypeMatchFrame *frame) {
    *frame=(TypeMatchFrame){0};
    uint32_t id=(uint32_t)expected;
    if (!c->count && c->source_types==c->types && expected==actual) return XR_XIR_OK;
    if (id>=XR_XIR_TYPE_PARAMETER_BASE && id<XR_XIR_TYPE_PARAMETER_LIMIT) {
        uint32_t index=id-XR_XIR_TYPE_PARAMETER_BASE;
        if (index>=c->count || !c->arguments) return XR_XIR_BAD_TYPE;
        TypeMatchContext receiver=*c;
        receiver.source_types=c->types;receiver.arguments=NULL;receiver.count=0;
        return type_match_pair(&receiver,c->arguments[index],actual,frame);
    }
    const XrXirTypeNode *from=xr_xir_type_node(c->source_types,expected);
    const XrXirTypeNode *to=xr_xir_type_node(c->types,actual);
    if ((from && from->kind==XR_XIR_TYPE_UNION) || (to && to->kind==XR_XIR_TYPE_UNION))
        return type_match_union(c,expected,actual,false,frame);
    if (!from) return expected==actual ? XR_XIR_OK : XR_XIR_BAD_TYPE;
    if (c->source_types==c->types && !from->parameter_span && expected==actual) return XR_XIR_OK;
    if (!to || from->kind!=to->kind || from->parameter_count!=to->parameter_count || from->flags!=to->flags)
        return XR_XIR_BAD_TYPE;
    if (from->kind==XR_XIR_TYPE_NOMINAL) {
        bool identity=c->nominal_match ? c->nominal_match(c->owner,c->source_types,from,c->types,to) :
            from->nominal.declaration==to->nominal.declaration;
        if (!identity || from->nominal.argument_count!=to->nominal.argument_count) return XR_XIR_BAD_TYPE;
    } else if (from->kind!=XR_XIR_TYPE_ARRAY && from->kind!=XR_XIR_TYPE_CELL &&
        from->kind!=XR_XIR_TYPE_CALLABLE && from->kind!=XR_XIR_TYPE_NULLABLE && from->kind!=XR_XIR_TYPE_TUPLE &&
        from->kind!=XR_XIR_TYPE_ATOMIC && from->kind!=XR_XIR_TYPE_TASK && from->kind!=XR_XIR_TYPE_MAP &&
        from->kind!=XR_XIR_TYPE_SET && from->kind!=XR_XIR_TYPE_SLICE && from->kind!=XR_XIR_TYPE_MUT_SLICE &&
        from->kind!=XR_XIR_TYPE_ITERATOR && from->kind!=XR_XIR_TYPE_CHANNEL) return XR_XIR_BAD_TYPE;
    *frame=(TypeMatchFrame){.from=from,.to=to,.source_types=c->source_types,
        .arguments=c->arguments,.argument_count=c->count};
    return XR_XIR_OK;
}
static void type_match_pop(TypeMatchFrame *stack,uint32_t *depth,XirReadScratch *read,bool success) {
    TypeMatchFrame *frame=&stack[--*depth];
    xir_read_scratch_release(read,frame->errors);*frame=(TypeMatchFrame){0};
    if (success && *depth) {
        TypeMatchFrame *parent=&stack[*depth-1];
        if (parent->errors && parent->awaiting) {
            parent->awaiting=false;++parent->next;parent->candidate=0;
        }
    }
}
static XrXirStatus type_match_next(TypeMatchContext *base,TypeMatchFrame *frame,TypeMatchFrame *child,bool *done) {
    TypeMatchContext c=*base;c.source_types=frame->source_types;
    c.arguments=frame->arguments;c.count=frame->argument_count;
    const XrXirTypeNode *from=frame->from,*to=frame->to;
    *done=false;*child=(TypeMatchFrame){0};
    if (frame->errors) {
        TypeMatchUnion *sets=frame->errors;
        if (frame->next==sets->count[frame->side]) {
            if (frame->side || sets->subset) {*done=true;return XR_XIR_OK;}
            frame->side=1;frame->next=0;frame->candidate=0;
        }
        if (frame->candidate==sets->count[1-frame->side]) return XR_XIR_BAD_TYPE;
        TypeMatchTerm left=sets->terms[0][frame->side?frame->candidate:frame->next];
        TypeMatchTerm right=sets->terms[1][frame->side?frame->next:frame->candidate];
        if (left.substituted) {c.source_types=c.types;c.arguments=NULL;c.count=0;}
        XrXirStatus status=type_match_pair(&c,left.type,right.type,child);
        if (status==XR_XIR_BAD_TYPE) {++frame->candidate;return XR_XIR_OK;}
        if (status!=XR_XIR_OK) return status;
        if (child->from || child->errors) frame->awaiting=true;
        else {++frame->next;frame->candidate=0;}
        return XR_XIR_OK;
    }
    uint32_t components=from->kind==XR_XIR_TYPE_NOMINAL ? from->nominal.argument_count :
        from->kind==XR_XIR_TYPE_CALLABLE ? from->parameter_count+2 :
        from->kind==XR_XIR_TYPE_TUPLE ? from->parameter_count :
        (from->kind==XR_XIR_TYPE_MAP || from->kind==XR_XIR_TYPE_TASK || from->kind==XR_XIR_TYPE_ITERATOR) ? 2 : 1;
    if (frame->next==components) {*done=true;return XR_XIR_OK;}
    XrXirType left=from->element,right=to->element;
    if ((from->kind==XR_XIR_TYPE_MAP || from->kind==XR_XIR_TYPE_TASK || from->kind==XR_XIR_TYPE_ITERATOR) && frame->next) {
        left=from->result;right=to->result;
    } else if (from->kind==XR_XIR_TYPE_NOMINAL) {
        left=from->nominal.arguments[frame->next];right=to->nominal.arguments[frame->next];
    } else if (from->kind==XR_XIR_TYPE_TUPLE) {
        uint32_t p=frame->next;
        if (from->parameters[p].mode || to->parameters[p].mode) return XR_XIR_BAD_TYPE;
        left=from->parameters[p].type;right=to->parameters[p].type;
    } else if (from->kind==XR_XIR_TYPE_CALLABLE) {
        if (!frame->next) {left=from->result;right=to->result;}
        else if (frame->next<=from->parameter_count) {
            uint32_t p=frame->next-1;
            if (from->parameters[p].mode!=to->parameters[p].mode) return XR_XIR_BAD_TYPE;
            left=from->parameters[p].type;right=to->parameters[p].type;
        }
    }
    ++frame->next;return type_match_pair(&c,left,right,child);
}
static XrXirStatus type_match_run_mode(const XrXirCompileContext *context,
    const XirTypeMatchRead *request,XrXirTypeMatchScratch *scratch,bool subset,
    void *owner,XrXirNominalMatch nominal_match) {
    if (!xir_compile_context_valid(context) || !request || !scratch || scratch->resources!=context->resources ||
        (request->argument_count && !request->arguments)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext remaining=*context;
    TypeMatchContext c={request->source_types,request->actual_types,request->arguments,
        request->argument_count,&remaining,scratch,request->memory,owner,nominal_match};
    TypeMatchFrame root={0};
    XrXirStatus status=subset ? type_match_union(&c,request->expected,request->actual,true,&root) :
        type_match_pair(&c,request->expected,request->actual,&root);
    if (status!=XR_XIR_OK || (!root.from && !root.errors)) return status;
    uint64_t maximum=(uint64_t)(c.source_types?c.source_types->count:0)+(c.types?c.types->count:0)+2;
    if (maximum>UINT32_MAX) {xir_read_scratch_release(c.read,root.errors);return XR_XIR_OVERFLOW;}
    TypeMatchMemory *claimed=NULL;
    status=type_match_stack_claim(&remaining,scratch,maximum<8?(uint32_t)maximum:8,&claimed,c.read);
    if (status!=XR_XIR_OK) {xir_read_scratch_release(c.read,root.errors);return status;}
    TypeMatchFrame *stack=claimed->frames;uint32_t depth=1;stack[0]=root;
    while (depth) {
        if (status==XR_XIR_BAD_TYPE) {
            while (depth && !(stack[depth-1].errors && stack[depth-1].awaiting))
                type_match_pop(stack,&depth,c.read,false);
            if (!depth) break;
            stack[depth-1].awaiting=false;++stack[depth-1].candidate;status=XR_XIR_OK;
        }
        if (status!=XR_XIR_OK) break;
        TypeMatchFrame child={0};bool done=false;
        status=type_match_next(&c,&stack[depth-1],&child,&done);
        if (status!=XR_XIR_OK) continue;
        if (done) {type_match_pop(stack,&depth,c.read,true);continue;}
        if (!child.from && !child.errors) continue;
        if (depth>=maximum) {xir_read_scratch_release(c.read,child.errors);status=XR_XIR_BAD_TYPE;continue;}
        if (depth==claimed->capacity) {
            uint32_t capacity=claimed->capacity>maximum/2?(uint32_t)maximum:claimed->capacity*2;
            status=type_match_stack_grow(&remaining,scratch,&claimed,capacity,depth,c.read);
            if (status!=XR_XIR_OK) {xir_read_scratch_release(c.read,child.errors);break;}
            stack=claimed->frames;
        }
        stack[depth++]=child;
    }
    while (depth) type_match_pop(stack,&depth,c.read,false);
    claimed->claimed=false;return status;
}
static XrXirStatus type_match_run(const XrXirCompileContext *context,
    const XirTypeMatchRead *request,XrXirTypeMatchScratch *scratch) {
    return type_match_run_mode(context,request,scratch,false,NULL,NULL);
}
XR_FUNC XrXirStatus xr_xir_compile_error_substitution_accepts(const XrXirCompileContext *context,
    const XrXirErrorMatch *match) {
    if (!xir_compile_context_valid(context) || !match) return XR_XIR_BAD_STRUCTURE;
    XirTypeMatchRead request={match->source_types,match->actual_types,match->arguments,
        match->argument_count,match->bound,match->actual,NULL};
    XrXirTypeMatchScratch scratch={context->resources,NULL};
    XrXirStatus status=type_match_run_mode(context,&request,&scratch,true,match->owner,match->nominal_match);
    type_match_scratch_free(&scratch,NULL);return status;
}
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches_between_scratch(const XrXirCompileContext *context,const XrXirTypes *source,const XrXirTypes *actual,const XrXirType *arguments,uint32_t count,XrXirType expected,XrXirType value,XrXirTypeMatchScratch *scratch) {
    XirTypeMatchRead request={source,actual,arguments,count,expected,value,NULL};
    return type_match_run(context,&request,scratch);
}
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches_between(const XrXirCompileContext *compile_context, const XrXirTypes *source_types, const XrXirTypes *types, const XrXirType *arguments, uint32_t count, XrXirType expected, XrXirType actual) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch = {compile_context->resources, NULL};
    XrXirStatus status = xr_xir_compile_type_substitution_matches_between_scratch(
        compile_context, source_types, types, arguments, count, expected, actual, &scratch);
    xr_xir_type_match_scratch_free(&scratch);
    return status;
}
XR_FUNC XrXirStatus xir_type_match_read(const XrXirCompileContext *context,const XirTypeMatchRead *request) {
    if(!xir_compile_context_valid(context) || !request) return XR_XIR_BAD_STRUCTURE;
    XrXirTypeMatchScratch scratch={context->resources,NULL};
    XrXirStatus status=type_match_run(context,request,&scratch);
    type_match_scratch_free(&scratch,request->memory);return status;
}
XR_FUNC XrXirStatus xr_xir_compile_type_substitution_matches(const XrXirCompileContext *compile_context, const XrXirTypes *types, const XrXirType *arguments, uint32_t count, XrXirType expected, XrXirType actual) {
    if (!xir_compile_context_valid(compile_context)) return XR_XIR_BAD_STRUCTURE;
    XrXirCompileContext compile_state = *compile_context;
    XrXirCompileContext *remaining = &compile_state;
    return xr_xir_compile_type_substitution_matches_between(remaining, types, types, arguments, count, expected, actual);
}
