/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_source_pattern_coverage.inc.c - Budgeted constructor matrix coverage
 *
 * KEY CONCEPT:
 *   Specialization preserves correlations between fields in each unguarded row.
 */
typedef struct SourcePatternMatrix {
    const XrXirType *types;
    SourceMatchPattern *const *patterns;
    uint32_t width, rows;
} SourcePatternMatrix;
static bool source_pattern_any(const SourceMatchPattern *pattern) {
    return !pattern || pattern->any;
}
static bool source_pattern_cut(SourceContext *ctx, uint64_t *cuts, uint32_t *count, uint64_t key) {
    uint32_t at=0;
    while (at<*count) {
        if (!source_work(ctx,NULL)) return false;
        if (cuts[at]==key) return true;
        if (cuts[at]>key) break;
        ++at;
    }
    for (uint32_t i=*count;i>at;--i) {
        if (!source_work(ctx,NULL)) return false;
        cuts[i]=cuts[i-1];
    }
    cuts[at]=key; ++*count; return true;
}
static bool source_pattern_integer_cuts(SourceContext *ctx, SourcePatternMatrix input,
    uint64_t **output, uint32_t *count) {
    if (input.rows>(UINT32_MAX-1)/2) return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern cut capacity overflow");
    uint64_t *cuts=source_alloc(ctx,(size_t)input.rows*2+1,sizeof(*cuts));
    if (!cuts) return false;
    *count=1; cuts[0]=0;
    uint64_t maximum=source_pattern_integer_max(input.types[0]);
    for (uint32_t r=0;r<input.rows;++r) {
        if (!source_work(ctx,NULL)) return false;
        SourceMatchPattern *head=input.patterns[(size_t)r*input.width];
        if (source_pattern_any(head) || !head->interval || head->empty) continue;
        if (!source_pattern_cut(ctx,cuts,count,head->low) ||
            (head->high<maximum && !source_pattern_cut(ctx,cuts,count,head->high+1))) return false;
    }
    *output=cuts; return true;
}
static bool source_pattern_coverage(SourceContext *ctx, SourcePatternMatrix input, uint32_t depth, bool *complete) {
    const XrXirType *types=input.types;
    SourceMatchPattern *const *matrix=input.patterns;
    uint32_t width=input.width, rows=input.rows;
    *complete=false;
    if (!source_work(ctx,NULL)) return false;
    if (!rows) return true;
    if (!width) { *complete=true; return true; }
    if (depth>=128) return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern coverage depth exhausted");
    for (uint32_t r=0;r<rows;++r) {
        bool any=true;
        for (uint32_t c=0;c<width;++c) {
            if (!source_work(ctx,NULL)) return false;
            if (!source_pattern_any(matrix[(size_t)r*width+c])) {any=false;break;}
        }
        if (any) {*complete=true;return true;}
    }
    bool enumeration=xr_xir_type_is_enum(&ctx->types,types[0]);
    bool boolean=types[0]==XR_XIR_BOOL;
    bool integer=xr_xir_type_is_integer(types[0]);
    uint64_t *cuts=NULL;
    uint32_t constructors=1;
    if (enumeration) constructors=ctx->nominals.declarations[xr_xir_type_node(&ctx->types,types[0])->nominal.declaration].variant_count;
    else if (boolean) constructors=2;
    else if (integer && !source_pattern_integer_cuts(ctx,input,&cuts,&constructors)) return false;
    for (uint32_t v=0;v<constructors;++v) {
        if (!source_work(ctx,NULL)) return false;
        uint32_t fields=0; XrXirType *field_types=NULL;
        if (enumeration && !source_pattern_field_types(ctx,types[0],v,&field_types,&fields)) return false;
        if (fields>UINT32_MAX-(width-1)) return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern matrix width overflow");
        uint32_t next_width=fields+width-1, next_rows=0;
        if (next_width && rows>SIZE_MAX/next_width)
            return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern matrix size overflow");
        XrXirType *next_types=next_width?source_alloc(ctx,next_width,sizeof(*next_types)):NULL;
        SourceMatchPattern **next=next_width?source_alloc(ctx,(size_t)rows*next_width,sizeof(*next)):NULL;
        if (next_width && (!next_types || !next)) return false;
        for (uint32_t c=0;c<next_width;++c) {
            if (!source_work(ctx,NULL)) return false;
            next_types[c]=c<fields?field_types[c]:types[c-fields+1];
        }
        for (uint32_t r=0;r<rows;++r) {
            if (!source_work(ctx,NULL)) return false;
            SourceMatchPattern *head=matrix[(size_t)r*width]; bool any=source_pattern_any(head);
            bool selected=any;
            if (!any && enumeration) selected=head->selection.variant==v;
            else if (!any && boolean) selected=head->boolean && head->truth==(v!=0);
            else if (!any && integer) {
                uint64_t high=v+1<constructors?cuts[v+1]-1:source_pattern_integer_max(types[0]);
                selected=head->interval && !head->empty && head->low<=cuts[v] && head->high>=high;
            }
            if (!selected) continue;
            for (uint32_t c=0;c<next_width;++c) {
                if (!source_work(ctx,NULL)) return false;
                next[(size_t)next_rows*next_width+c]=c<fields?(any?NULL:head->fields[c]):matrix[(size_t)r*width+c-fields+1];
            }
            ++next_rows;
        }
        bool covered=false;
        if (!source_pattern_coverage(ctx,(SourcePatternMatrix){next_types,next,next_width,next_rows},depth+1,&covered)) return false;
        if (!covered) return true;
    }
    *complete=true; return true;
}
