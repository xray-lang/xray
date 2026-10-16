/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_source_call_syntax.inc.c - Owned observations of completed parser calls
 *
 * KEY CONCEPT:
 *   The parser identifies separators. Final AST reachability rejects discarded
 *   speculative observations; consumers receive only numeric syntax facts.
 */

static int syntax_call_compare_position(XrNameSpan a,XrNameSpan b) {
    return a.line<b.line?-1:a.line>b.line?1:a.column<b.column?-1:a.column>b.column?1:0;
}

static XrCompileResourceStatus syntax_call_selection(XlspSyntaxSnapshot *s,
    const AstNode *callee,XlspSyntaxCallRecord *record) {
    while(callee&&callee->type==AST_FUNCTION_REF) {
        XrCompileResourceStatus status=xr_compile_state_work(s->state,1);
        if(status!=XR_COMPILE_RESOURCE_OK)return status;
        callee=callee->as.function_ref.callee;
    }
    const char *name=NULL;XrNameSpan start={0};
    if(callee&&callee->type==AST_VARIABLE) {
        name=callee->as.variable.name;start=(XrNameSpan){callee->line,callee->column};
    } else if(callee&&callee->type==AST_MEMBER_ACCESS) {
        name=callee->as.member_access.name;start=callee->as.member_access.name_span;
    }
    if(!name||start.line<=0||start.column<=0)return XR_COMPILE_RESOURCE_OK;
    size_t length=0;
    for(;;++length) {
        XrCompileResourceStatus status=xr_compile_state_work(s->state,1);
        if(status!=XR_COMPILE_RESOURCE_OK)return status;
        if(!name[length])break;
        if(length>=(size_t)(INT_MAX-start.column))return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    }
    if(!length)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    record->has_callee=true;record->selection=start;
    record->selection_end=(XrNameSpan){start.line,start.column+(int)length};
    return XR_COMPILE_RESOURCE_OK;
}

static XrCompileResourceStatus syntax_call_observe(void *context,const XrParseCallSyntax *syntax) {
    XlspSyntaxSnapshot *s=context;
    if(!s||!syntax||!syntax->node||!syntax->boundaries||!syntax->boundary_count||
        syntax->boundary_count>SIZE_MAX/sizeof(XrNameSpan))return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    const AstNode *node=syntax->node;
    int arguments=node->type==AST_CALL_EXPR?node->as.call_expr.arg_count:
        node->type==AST_NEW_EXPR?node->as.new_expr.arg_count:-1;
    if(arguments<0)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if(syntax->boundary_count>(size_t)arguments+1)return XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    if(s->call_count==s->call_capacity) {
        if(s->call_capacity>SIZE_MAX/2/sizeof(*s->calls))return XR_COMPILE_RESOURCE_BUDGET;
        size_t capacity=s->call_capacity?s->call_capacity*2:16;
        XrCompileResourceStatus status=xr_compile_state_resize(s->state,(void **)&s->calls,
            capacity*sizeof(*s->calls));
        if(status!=XR_COMPILE_RESOURCE_OK)return status;
        s->call_capacity=capacity;
    }
    XlspSyntaxCallRecord record={0};
    record.address=(uintptr_t)node;record.node_id=node->node_id;record.type=node->type;
    record.start=(XrNameSpan){node->line,node->column};
    record.end=(XrNameSpan){node->end_line,node->end_column};record.closing=syntax->closing;
    record.argument_count=(size_t)arguments;record.boundary_count=syntax->boundary_count;
    XrCompileResourceStatus status=syntax_call_selection(s,syntax->callee,&record);
    if(status!=XR_COMPILE_RESOURCE_OK)return status;
    size_t bytes=record.boundary_count*sizeof(*record.boundaries);
    status=xr_compile_state_alloc(s->state,bytes,(void **)&record.boundaries);
    if(status!=XR_COMPILE_RESOURCE_OK)return status;
    status=xr_compile_state_copy(s->state,record.boundaries,syntax->boundaries,bytes);
    if(status==XR_COMPILE_RESOURCE_OK)status=xr_compile_state_work(s->state,sizeof(record));
    if(status==XR_COMPILE_RESOURCE_OK)s->calls[s->call_count++]=record;
    else xr_compile_state_free(record.boundaries);
    return status;
}

static XrCompileResourceStatus syntax_call_sift(XlspSyntaxSnapshot *s,size_t root,size_t count) {
    while(root<count/2) {
        XrCompileResourceStatus status=xr_compile_state_work(s->state,sizeof(*s->calls));
        if(status!=XR_COMPILE_RESOURCE_OK)return status;
        size_t child=root*2+1;
        if(child+1<count&&s->calls[child].address<s->calls[child+1].address)++child;
        if(s->calls[root].address>=s->calls[child].address)break;
        XlspSyntaxCallRecord temp=s->calls[root];s->calls[root]=s->calls[child];s->calls[child]=temp;
        root=child;
    }
    return XR_COMPILE_RESOURCE_OK;
}

typedef struct XlspSyntaxCallWalk {
    XlspSyntaxSnapshot *snapshot;
    const AstNode **stack;
    size_t count,capacity;
    XrXirStatus status;
} XlspSyntaxCallWalk;

static bool syntax_call_child(AstNode *node,void *context) {
    XlspSyntaxCallWalk *walk=context;
    walk->status=syntax_push(walk->snapshot,&walk->stack,&walk->count,&walk->capacity,node);
    return walk->status==XR_XIR_OK;
}

static XrCompileResourceStatus syntax_call_mark(XlspSyntaxSnapshot *s,const AstNode *node) {
    size_t low=0,high=s->call_count;uintptr_t address=(uintptr_t)node;
    while(low<high) {
        XrCompileResourceStatus status=xr_compile_state_work(s->state,1);
        if(status!=XR_COMPILE_RESOURCE_OK)return status;
        size_t mid=low+(high-low)/2;
        if(s->calls[mid].address<address)low=mid+1;else high=mid;
    }
    for(size_t i=low;i<s->call_count&&s->calls[i].address==address;++i) {
        XrCompileResourceStatus status=xr_compile_state_work(s->state,sizeof(*s->calls));
        if(status!=XR_COMPILE_RESOURCE_OK)return status;
        XlspSyntaxCallRecord *record=&s->calls[i];
        if(record->node_id!=node->node_id||record->type!=node->type)continue;
        int arguments=node->type==AST_CALL_EXPR?node->as.call_expr.arg_count:
            node->type==AST_NEW_EXPR?node->as.new_expr.arg_count:-1;
        if(arguments<0||record->argument_count!=(size_t)arguments||
            record->start.line!=node->line||record->start.column!=node->column||
            record->end.line!=node->end_line||record->end.column!=node->end_column)continue;
        XlspSyntaxCallRecord current={0};
        if(node->type==AST_CALL_EXPR) {
            status=syntax_call_selection(s,node->as.call_expr.callee,&current);
            if(status!=XR_COMPILE_RESOURCE_OK)return status;
            if(current.has_callee!=record->has_callee||
                syntax_call_compare_position(current.selection,record->selection)||
                syntax_call_compare_position(current.selection_end,record->selection_end))continue;
        }
        record->live=true;
    }
    return XR_COMPILE_RESOURCE_OK;
}

static XrCompileResourceStatus syntax_calls_finalize(XlspSyntaxSnapshot *s) {
    if(!s->call_count)return XR_COMPILE_RESOURCE_OK;
    XrCompileResourceStatus status=XR_COMPILE_RESOURCE_OK;
    for(size_t i=s->call_count/2;i&&status==XR_COMPILE_RESOURCE_OK;--i)
        status=syntax_call_sift(s,i-1,s->call_count);
    for(size_t count=s->call_count;count>1&&status==XR_COMPILE_RESOURCE_OK;--count) {
        status=xr_compile_state_work(s->state,sizeof(*s->calls));
        if(status!=XR_COMPILE_RESOURCE_OK)break;
        XlspSyntaxCallRecord temp=s->calls[0];s->calls[0]=s->calls[count-1];s->calls[count-1]=temp;
        status=syntax_call_sift(s,0,count-1);
    }
    XlspSyntaxCallWalk walk={0};walk.snapshot=s;walk.status=XR_XIR_OK;
    if(status==XR_COMPILE_RESOURCE_OK)(void)syntax_call_child(s->ast,&walk);
    while(walk.count&&walk.status==XR_XIR_OK&&status==XR_COMPILE_RESOURCE_OK) {
        const AstNode *node=walk.stack[--walk.count];
        status=xr_compile_state_work(s->state,sizeof(*node));
        if(status==XR_COMPILE_RESOURCE_OK)status=syntax_call_mark(s,node);
        if(status!=XR_COMPILE_RESOURCE_OK)break;
        if(!xr_ast_for_each_child(node,syntax_call_child,&walk)&&walk.status==XR_XIR_OK)
            walk.status=XR_XIR_BAD_STRUCTURE;
    }
    xr_compile_state_free(walk.stack);
    if(status==XR_COMPILE_RESOURCE_OK&&walk.status!=XR_XIR_OK)
        status=walk.status==XR_XIR_BUDGET?XR_COMPILE_RESOURCE_BUDGET:
            walk.status==XR_XIR_OUT_OF_MEMORY?XR_COMPILE_RESOURCE_OUT_OF_MEMORY:XR_COMPILE_RESOURCE_BAD_ARGUMENT;
    return status;
}

static XrXirStatus syntax_call_range(const XlspSyntaxSnapshot *s,XrNameSpan start,XrNameSpan end,
    XrLspRange *range) {
    XrCompileResources *resources=xr_compile_state_resources(s->state);
    XrXirStatus status=xlsp_source_position(resources,s->text,s->length,start.line,start.column,&range->start);
    if(status==XR_XIR_OK)status=xlsp_source_position(resources,s->text,s->length,end.line,end.column,&range->end);
    return status;
}

XR_FUNC XrXirStatus xlsp_source_syntax_call(const XlspSyntaxSnapshot *s,XrLspPosition position,
    XlspSourceCallSyntax *out) {
    if(!s||!out)return XR_XIR_BAD_STRUCTURE;
    XlspSourceCursor cursor={0};
    XrXirStatus status=xlsp_source_cursor(xr_compile_state_resources(s->state),s->text,s->length,position,&cursor);
    if(status!=XR_XIR_OK)return status;
    XrNameSpan at={cursor.line,cursor.column};const XlspSyntaxCallRecord *chosen=NULL;
    for(size_t i=0;i<s->call_count;++i) {
        status=syntax_work(s,sizeof(*s->calls));if(status!=XR_XIR_OK)return status;
        const XlspSyntaxCallRecord *record=&s->calls[i];
        if(!record->live||syntax_call_compare_position(record->boundaries[0],at)>0||
            syntax_call_compare_position(at,record->closing)>0)continue;
        if(!chosen||syntax_call_compare_position(record->boundaries[0],chosen->boundaries[0])>0||
            (syntax_call_compare_position(record->boundaries[0],chosen->boundaries[0])==0&&
             syntax_call_compare_position(record->closing,chosen->closing)<0))chosen=record;
    }
    if(!chosen)return XR_XIR_UNRESOLVED;
    XlspSourceCallSyntax result={0};result.has_callee=chosen->has_callee;
    result.argument_count=chosen->argument_count;
    for(size_t i=1;i<chosen->boundary_count;++i) {
        status=syntax_work(s,1);if(status!=XR_XIR_OK)return status;
        if(syntax_call_compare_position(chosen->boundaries[i],at)>0)break;
        result.active_argument=i;
    }
    status=syntax_call_range(s,chosen->boundaries[0],chosen->closing,&result.arguments);
    if(status==XR_XIR_OK&&chosen->has_callee)
        status=syntax_call_range(s,chosen->selection,chosen->selection_end,&result.callee);
    if(status==XR_XIR_OK)*out=result;
    return status;
}
