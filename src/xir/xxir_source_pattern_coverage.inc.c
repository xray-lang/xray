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
 *   Coverage and missing-value reports specialize through the same steps, so a
 *   reported value is exactly one the coverage decision found uncovered.
 */
typedef struct SourcePatternMatrix {
    const XrXirType *types;
    SourceMatchPattern *const *patterns;
    uint32_t width, rows;
} SourcePatternMatrix;
/* The head column's constructors; integers and arrays split at pattern cuts. */
typedef struct SourcePatternHead {
    uint64_t *cuts;
    XrXirType type;
    uint32_t constructors;
    bool enumeration, boolean, integer, array, product;
} SourcePatternHead;
/* One constructor's rows, with its fields in place of the head column. */
typedef struct SourcePatternSpecial {
    SourcePatternMatrix matrix;
    SourcePatternProduct *description;
    uint32_t fields;
} SourcePatternSpecial;
/* Diagnostics list three missing values; a fourth only proves more exist. */
enum { SOURCE_PATTERN_MISSING_LIMIT = 4 };
typedef struct SourcePatternMissing {
    const char **rows[SOURCE_PATTERN_MISSING_LIMIT];
    uint32_t count;
} SourcePatternMissing;
static bool source_pattern_any(const SourceMatchPattern *pattern) {
    return !pattern || pattern->any;
}
static bool source_pattern_cut(SourceContext *ctx, uint64_t *cuts, uint32_t *count, uint64_t key) {
    uint32_t at=0;
    while (at<*count) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (cuts[at]==key) return true;
        if (cuts[at]>key) break;
        ++at;
    }
    for (uint32_t i=*count;i>at;--i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
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
    uint64_t maximum=xr_xir_type_is_array(&ctx->types,input.types[0]) ? INT64_MAX : source_pattern_integer_max(input.types[0]);
    for (uint32_t r=0;r<input.rows;++r) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        SourceMatchPattern *head=input.patterns[(size_t)r*input.width];
        if (source_pattern_any(head) || !head->interval || head->empty) continue;
        if (!source_pattern_cut(ctx,cuts,count,head->low) ||
            (head->high<maximum && !source_pattern_cut(ctx,cuts,count,head->high+1))) return false;
    }
    *output=cuts; return true;
}
static bool source_pattern_covering_row(SourceContext *ctx, SourcePatternMatrix input, bool *found) {
    *found=false;
    for (uint32_t r=0;r<input.rows;++r) {
        bool any=true;
        for (uint32_t c=0;c<input.width;++c) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            if (!source_pattern_any(input.patterns[(size_t)r*input.width+c])) {any=false;break;}
        }
        if (any) {*found=true;return true;}
    }
    return true;
}
static bool source_pattern_free_head(SourceContext *ctx, SourcePatternMatrix input, bool *free_head) {
    *free_head=true;
    for (uint32_t r=0;r<input.rows;++r) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (!source_pattern_any(input.patterns[(size_t)r*input.width])) {*free_head=false;break;}
    }
    return true;
}
/* An unrestricted column cannot affect correlations in the remaining row. */
static bool source_pattern_default(SourceContext *ctx, SourcePatternMatrix input, SourcePatternMatrix *output) {
    uint32_t next_width=input.width-1, rows=input.rows;
    if (next_width && rows>SIZE_MAX/next_width)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern default matrix size overflow");
    SourceMatchPattern **next=next_width ? source_alloc(ctx,(size_t)rows*next_width,sizeof(*next)) : NULL;
    if (next_width && !next) return false;
    for (uint32_t r=0;r<rows;++r)
        for (uint32_t c=0;c<next_width;++c) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            next[(size_t)r*next_width+c]=input.patterns[(size_t)r*input.width+c+1];
        }
    *output=(SourcePatternMatrix){input.types+1,next,next_width,rows};
    return true;
}
static bool source_pattern_head(SourceContext *ctx, SourcePatternMatrix input, SourcePatternHead *head) {
    XrXirType type=input.types[0];
    *head=(SourcePatternHead){.type=type,.constructors=1};
    head->enumeration=xr_xir_type_is_enum(&ctx->types,type);
    head->boolean=type==XR_XIR_BOOL;
    head->integer=xr_xir_type_is_integer(type);
    head->array=xr_xir_type_is_array(&ctx->types,type);
    head->product=source_pattern_product_kind(ctx,type)!=XR_XIR_OP_COUNT;
    if (head->enumeration) head->constructors=ctx->nominals.declarations[xr_xir_type_node(&ctx->types,type)->nominal.declaration].variant_count;
    else if (head->boolean) head->constructors=2;
    else if ((head->integer || head->array) && !source_pattern_integer_cuts(ctx,input,&head->cuts,&head->constructors)) return false;
    return true;
}
static bool source_pattern_specialize(SourceContext *ctx, SourcePatternMatrix input,
    const SourcePatternHead *head, uint32_t v, SourcePatternSpecial *output) {
    const XrXirType *types=input.types;
    SourceMatchPattern *const *matrix=input.patterns;
    uint32_t width=input.width, rows=input.rows;
    uint32_t fields=0; XrXirType *field_types=NULL;
    if (head->enumeration && !source_pattern_field_types(ctx,types[0],v,&field_types,&fields)) return false;
    SourcePatternProduct *description=NULL;
    if (head->product) {
        if (!source_pattern_product_description(ctx,types[0],&description)) return false;
        fields=description->count;
    }
    if (fields>UINT32_MAX-(width-1)) return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern matrix width overflow");
    uint32_t next_width=fields+width-1, next_rows=0;
    if (next_width && rows>SIZE_MAX/next_width)
        return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern matrix size overflow");
    XrXirType *next_types=next_width?source_alloc(ctx,next_width,sizeof(*next_types)):NULL;
    SourceMatchPattern **next=next_width?source_alloc(ctx,(size_t)rows*next_width,sizeof(*next)):NULL;
    if (next_width && (!next_types || !next)) return false;
    for (uint32_t c=0;c<next_width;++c) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        next_types[c]=c<fields?(head->product?description->fields[c].type:field_types[c]):types[c-fields+1];
    }
    for (uint32_t r=0;r<rows;++r) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        SourceMatchPattern *pattern=matrix[(size_t)r*width]; bool any=source_pattern_any(pattern);
        bool selected=any;
        if (!any && head->enumeration) selected=pattern->selection.variant==v;
        else if (!any && head->product) selected=pattern->product!=NULL;
        else if (!any && head->boolean) selected=pattern->boolean && pattern->truth==(v!=0);
        else if (!any && (head->integer || head->array)) {
            uint64_t high=v+1<head->constructors?head->cuts[v+1]-1:(head->array?INT64_MAX:source_pattern_integer_max(types[0]));
            selected=pattern->interval && !pattern->empty && pattern->low<=head->cuts[v] && pattern->high>=high;
        }
        if (!selected) continue;
        for (uint32_t c=0;c<next_width;++c) {
            if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
            next[(size_t)next_rows*next_width+c]=c<fields?(any?NULL:pattern->fields[c]):matrix[(size_t)r*width+c-fields+1];
        }
        ++next_rows;
    }
    *output=(SourcePatternSpecial){{next_types,next,next_width,next_rows},description,fields};
    return true;
}
static bool source_pattern_coverage(SourceContext *ctx, SourcePatternMatrix input, uint32_t depth, bool *complete) {
    *complete=false;
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (!input.rows) return true;
    if (!input.width) { *complete=true; return true; }
    if (depth>=128) return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern coverage depth exhausted");
    bool found=false;
    if (!source_pattern_covering_row(ctx,input,&found)) return false;
    if (found) {*complete=true;return true;}
    if (!source_pattern_free_head(ctx,input,&found)) return false;
    if (found) {
        SourcePatternMatrix next;
        if (!source_pattern_default(ctx,input,&next)) return false;
        return source_pattern_coverage(ctx,next,depth+1,complete);
    }
    SourcePatternHead head;
    if (!source_pattern_head(ctx,input,&head)) return false;
    for (uint32_t v=0;v<head.constructors;++v) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        SourcePatternSpecial special; bool covered=false;
        if (!source_pattern_specialize(ctx,input,&head,v,&special) ||
            !source_pattern_coverage(ctx,special.matrix,depth+1,&covered)) return false;
        if (!covered) return true;
    }
    *complete=true; return true;
}
/* Closed scrutinees must match exhaustively: enums, bool, and tuples whose
 * elements are all closed. Integers, text, arrays and nominal products stay open. */
static bool source_pattern_closed(SourceContext *ctx, XrXirType type, uint32_t depth, bool *closed) {
    *closed=false;
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (depth>=128) return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern closure depth exhausted");
    if (xr_xir_type_is_enum(&ctx->types,type) || type==XR_XIR_BOOL) {*closed=true;return true;}
    if (source_pattern_product_kind(ctx,type)!=XR_XIR_TUPLE_FIELD) return true;
    SourcePatternProduct *product=NULL;
    if (!source_pattern_product_description(ctx,type,&product)) return false;
    for (uint32_t f=0;f<product->count;++f) {
        bool element=false;
        if (!source_pattern_closed(ctx,product->fields[f].type,depth+1,&element)) return false;
        if (!element) return true;
    }
    *closed=true; return true;
}
static bool source_pattern_text(SourceContext *ctx, const XrXirLiteral *parts, uint32_t count, const char **output) {
    size_t length=0;
    for (uint32_t i=0;i<count;++i) {
        if (parts[i].length>SIZE_MAX-1-length) return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern text budget exhausted");
        length+=parts[i].length;
    }
    char *text=source_alloc(ctx,length+1,1);
    if (!text) return false;
    size_t at=0;
    for (uint32_t i=0;i<count;++i) {
        if (parts[i].length) memcpy(text+at,parts[i].bytes,parts[i].length);
        at+=parts[i].length;
    }
    text[at]=0; *output=text; return true;
}
static XrXirLiteral source_pattern_literal(const char *text) {
    return (XrXirLiteral){text,(uint32_t)strlen(text)};
}
/* Renders constructor v around the first special->fields columns of a missing
 * row. A constructor that no row selects is missing whole and shows no fields;
 * named payloads and records show only the fields that still constrain. */
static bool source_pattern_constructor_text(SourceContext *ctx, const SourcePatternHead *head,
    const SourcePatternSpecial *special, uint32_t v, const char *const *columns, const char **output) {
    if (head->boolean) {*output=v?"true":"false";return true;}
    if (!head->enumeration && !head->product) {*output="_";return true;}
    bool tuple=head->product && special->description->operation==XR_XIR_TUPLE_FIELD;
    const XrXirNominalDeclaration *d=head->enumeration ?
        &ctx->nominals.declarations[xr_xir_type_node(&ctx->types,head->type)->nominal.declaration] : NULL;
    uint32_t fields=special->fields, shown=0;
    if (head->enumeration && (!fields || !special->matrix.rows))
        return source_pattern_text(ctx,(XrXirLiteral[]){d->name,{".",1},d->variants[v].name},3,output);
    for (uint32_t f=0;f<fields;++f) shown+=tuple || strcmp(columns[f],"_")!=0;
    bool all=!shown;
    if (fields>(UINT32_MAX-5)/4) return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern text budget exhausted");
    XrXirLiteral *parts=source_alloc(ctx,(size_t)fields*4+5,sizeof(*parts));
    if (!parts) return false;
    uint32_t count=0;
    if (d) {parts[count++]=d->name;parts[count++]=(XrXirLiteral){".",1};parts[count++]=d->variants[v].name;}
    parts[count++]=source_pattern_literal(tuple?"(":d?" { ":"{ ");
    bool first=true;
    for (uint32_t f=0;f<fields;++f) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        if (!tuple && !all && !strcmp(columns[f],"_")) continue;
        if (!first) parts[count++]=(XrXirLiteral){", ",2};
        first=false;
        if (!tuple) {
            XrXirLiteral name=d ? d->fields[d->variants[v].field_begin+f].name : special->description->fields[f].name;
            parts[count++]=name; parts[count++]=(XrXirLiteral){": ",2};
        }
        parts[count++]=source_pattern_literal(columns[f]);
    }
    parts[count++]=source_pattern_literal(!tuple?" }":fields==1?",)":")");
    return source_pattern_text(ctx,parts,count,output);
}
static bool source_pattern_missing_add(SourceContext *ctx, SourcePatternMissing *out, const char **row, uint32_t width) {
    for (uint32_t i=0;i<out->count;++i) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        bool same=true;
        for (uint32_t c=0;c<width && same;++c) same=!strcmp(out->rows[i][c],row[c]);
        if (same) return true;
    }
    if (out->count<SOURCE_PATTERN_MISSING_LIMIT) out->rows[out->count++]=row;
    return true;
}
/* Collects up to SOURCE_PATTERN_MISSING_LIMIT distinct uncovered rows, one
 * rendered column per matrix column, in declaration order. Open columns render
 * as `_`, so distinct integer intervals can collapse into one row. */
static bool source_pattern_missing(SourceContext *ctx, SourcePatternMatrix input, uint32_t depth,
    SourcePatternMissing *out) {
    uint32_t width=input.width;
    out->count=0;
    if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
    if (depth>=128) return source_fail(ctx,NULL,XR_XIR_BUDGET,"pattern coverage depth exhausted");
    if (!input.rows) {
        const char **row=width?source_alloc(ctx,width,sizeof(*row)):NULL;
        if (width && !row) return false;
        for (uint32_t c=0;c<width;++c) row[c]="_";
        out->rows[out->count++]=row; return true;
    }
    if (!width) return true;
    bool found=false;
    if (!source_pattern_covering_row(ctx,input,&found)) return false;
    if (found) return true;
    if (!source_pattern_free_head(ctx,input,&found)) return false;
    if (found) {
        SourcePatternMatrix next; SourcePatternMissing rest;
        if (!source_pattern_default(ctx,input,&next) || !source_pattern_missing(ctx,next,depth+1,&rest)) return false;
        for (uint32_t i=0;i<rest.count;++i) {
            const char **row=source_alloc(ctx,width,sizeof(*row));
            if (!row) return false;
            row[0]="_";
            for (uint32_t c=1;c<width;++c) row[c]=rest.rows[i][c-1];
            if (!source_pattern_missing_add(ctx,out,row,width)) return false;
        }
        return true;
    }
    SourcePatternHead head;
    if (!source_pattern_head(ctx,input,&head)) return false;
    for (uint32_t v=0;v<head.constructors && out->count<SOURCE_PATTERN_MISSING_LIMIT;++v) {
        if (!((ctx)->diagnostic.status == XR_XIR_OK)) return false;
        SourcePatternSpecial special; SourcePatternMissing rest;
        if (!source_pattern_specialize(ctx,input,&head,v,&special) ||
            !source_pattern_missing(ctx,special.matrix,depth+1,&rest)) return false;
        for (uint32_t i=0;i<rest.count && out->count<SOURCE_PATTERN_MISSING_LIMIT;++i) {
            const char **row=source_alloc(ctx,width,sizeof(*row));
            if (!row || !source_pattern_constructor_text(ctx,&head,&special,v,rest.rows[i],&row[0])) return false;
            for (uint32_t c=1;c<width;++c) row[c]=rest.rows[i][special.fields+c-1];
            if (!source_pattern_missing_add(ctx,out,row,width)) return false;
        }
    }
    return true;
}
