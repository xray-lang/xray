/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_records.inc.c - Lossless affine physical instruction runs
 *
 * KEY CONCEPT:
 *   Unsigned arithmetic preserves all ten physical words, including unused
 *   words and the exact immediate bits. No type or opcode selects a codec.
 */
static uint32_t effect_body_source_word(const XrXirInstruction *op,uint32_t word) {
    switch (word) {
    case 0:return (uint32_t)op->op;
    case 1:return op->type;
    case 2:return op->args[0];case 3:return op->args[1];
    case 4:return op->targets[0];case 5:return op->targets[1];
    case 6:return (uint32_t)(uint64_t)op->immediate;
    case 7:return (uint32_t)((uint64_t)op->immediate>>32);
    case 8:return op->type_arguments[0];case 9:return op->type_arguments[1];
    default:return 0;
    }
}

/* Unused capacity is never read or traversed. A failed growth retains the
 * previous allocation so the unique material destructor can release it. */
static XrXirStatus effect_body_runs_reserve(const XrXirCompileContext *work,
    EffectBodyFunction *function) {
    if (!function || function->run_count>function->run_capacity ||
        function->run_count>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (function->run_count<function->run_capacity) return XR_XIR_OK;
    uint32_t capacity=function->run_capacity?function->run_capacity:4;
    if (capacity<=function->run_count) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        capacity=capacity>UINT32_MAX/2?function->instruction_count:capacity*2;
    }
    if (capacity>function->instruction_count) capacity=function->instruction_count;
    if ((uint64_t)capacity>SIZE_MAX/sizeof(*function->runs)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    EffectBodyRun *storage=xir_compile_alloc(work,(size_t)capacity*sizeof(*storage),&status);
    if (!storage) return status;
    if (!xir_compile_work(work,3)) { xr_compile_resources_free(storage);return XR_XIR_BUDGET; }
    /* Only initialized base fields and mask-selected deltas move. No unused
     * slot, inactive delta or capacity tail is copied or observed. */
    for (uint32_t r=0;r<function->run_count;++r) {
        const EffectBodyRun *source=&function->runs[r];EffectBodyRun *target=&storage[r];
        if (!xir_compile_work(work,3*sizeof(uint32_t)+sizeof(source->base))) {
            xr_compile_resources_free(storage);return XR_XIR_BUDGET;
        }
        target->first=source->first;target->count=source->count;target->mask=source->mask;
        target->base=source->base;
        for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
            if (!xir_compile_work(work,1)) { xr_compile_resources_free(storage);return XR_XIR_BUDGET; }
            if (!(source->mask&(UINT32_C(1)<<w))) continue;
            if (!xir_compile_work(work,sizeof(uint32_t))) {
                xr_compile_resources_free(storage);return XR_XIR_BUDGET;
            }
            target->delta[w]=source->delta[w];
        }
    }
    xr_compile_resources_free(function->runs);function->runs=storage;
    function->run_capacity=capacity;return XR_XIR_OK;
}

static inline XrXirStatus effect_body_run_append(const XrXirCompileContext *work,
    EffectBodyFunction *function,const EffectBodyRun *run) {
    if (!function || !run || function->run_count>function->run_capacity ||
        function->run_capacity>function->instruction_count ||
        (function->run_capacity && !function->runs)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,3+2*sizeof(uintptr_t))) return XR_XIR_BUDGET;
    /* A borrowed record must not survive reserve reallocation as its input.
     * This is a memory-alias boundary, never a permission or identity proof. */
    uintptr_t address=(uintptr_t)run,begin=(uintptr_t)function->runs;
    if (function->runs && address>=begin &&
        (address-begin)/sizeof(*run)<function->run_capacity) return XR_XIR_BAD_STRUCTURE;
    if (!run->count || run->first>=function->instruction_count ||
        run->count>function->instruction_count-run->first || run->mask&~EFFECT_BODY_MASK)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (!(run->mask&(UINT32_C(1)<<w))) continue;
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (run->count==1 || !run->delta[w]) return XR_XIR_BAD_STRUCTURE;
    }
    XrXirStatus status=effect_body_runs_reserve(work,function);
    if (status!=XR_XIR_OK) return status;
    EffectBodyRun *target=&function->runs[function->run_count];
    if (!xir_compile_work(work,3*sizeof(uint32_t)+sizeof(run->base)+1)) return XR_XIR_BUDGET;
    target->first=run->first;target->count=run->count;target->mask=run->mask;
    target->base=run->base;
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (!(run->mask&(UINT32_C(1)<<w))) continue;
        if (!xir_compile_work(work,sizeof(uint32_t))) return XR_XIR_BUDGET;
        target->delta[w]=run->delta[w];
    }
    ++function->run_count;
    if (run->first+run->count==function->instruction_count) {
        if (!xir_compile_work(work,sizeof(function->run_capacity))) return XR_XIR_BUDGET;
        function->run_capacity=function->run_count;
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_body_run_seed(const XrXirCompileContext *work,
    EffectBodyRun *run,const XrXirInstruction *source,uint32_t first) {
    if (!xir_compile_work(work,3+EFFECT_BODY_WORDS*(1+sizeof(uint32_t)))) return XR_XIR_BUDGET;
    run->first=first;run->count=1;run->mask=0;
    run->base=*source;
    return XR_XIR_OK;
}

typedef struct EffectBodyWriteSink {
    void *owner;
    XrXirStatus (*row)(void *,uint32_t,const XrXirInstruction *);
    XrXirStatus (*span)(void *,uint32_t,uint32_t);
} EffectBodyWriteSink;

/* Semantic intake follows the complete physical comparison and append.
 * A constant-column mask chooses only the private range reader. That reader
 * independently checks the real fields; all other rows retain scalar intake. */
static XrXirStatus effect_body_run_submit(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyFunction *target,const EffectBodyRun *run,
    const EffectBodyWriteSink *sink) {
    if (!source || !target || !target->runs || !run || run!=&target->runs[target->run_count] ||
        target->run_count>=target->run_capacity || !run->count ||
        run->first>=target->instruction_count || run->count>target->instruction_count-run->first)
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,1+3*sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t first=run->first,count=run->count,mask=run->mask;
    ++target->run_count;
    if (!sink) return XR_XIR_OK;
    if (!sink->owner || !sink->row || !sink->span) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
    if (count>1 && !(mask&UINT32_C(3))) return sink->span(sink->owner,first,count);
    for (uint32_t i=first;i<first+count;++i) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        XrXirStatus status=sink->row(sink->owner,i,&source->instructions[i]);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* A zero or single varying column selects only an encoding comparison.
 * Every physical word is still compared. No column index is a type, role,
 * permission or proof, and it never survives this run's local request. */
static XrXirStatus effect_body_single_column(const EffectBodyRead *read,uint32_t mask,uint32_t *output) {
    if (!output || (mask&~EFFECT_BODY_MASK)) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,3+sizeof(*output))) return XR_XIR_BUDGET;
    if (!mask) { *output=EFFECT_BODY_WORDS;return XR_XIR_OK; }
    if (mask&(mask-1)) { *output=UINT32_MAX;return XR_XIR_OK; }
    uint32_t word=0;
    while ((mask>>word)!=1) {
        if (!effect_body_read_work(read,3)) return XR_XIR_BUDGET;
        ++word;
    }
    *output=word;return XR_XIR_OK;
}

/* A matching constant opcode equals the already validated run base. A
 * varying opcode and every new row still pass the same range gate. On a
 * field mismatch the new seed copies all ten words before semantic intake;
 * partial comparisons never publish an instruction or authority product. */
static XrXirStatus effect_body_records_write(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyFunction *target,const EffectBodyWriteSink *sink) {
    if (!source || !target || source->instruction_count!=target->instruction_count ||
        target->run_count || (source->instruction_count && !source->instructions))
        return XR_XIR_BAD_STRUCTURE;
    if (!source->instruction_count) return XR_XIR_OK;
    if (!xir_compile_work(work,sizeof(uint32_t)+sizeof(EffectBodyRun *))) return XR_XIR_BUDGET;
    uint32_t column=UINT32_MAX;EffectBodyRun *run=NULL;
    for (uint32_t i=0;i<source->instruction_count;++i) {
        const XrXirInstruction *op=&source->instructions[i];
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (!run || run->count<2 || (run->mask&UINT32_C(1))) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        }
        if (!run) {
            XrXirStatus status=effect_body_runs_reserve(work,target);
            if (status!=XR_XIR_OK) return status;
            if (!xir_compile_work(work,sizeof(run))) return XR_XIR_BUDGET;
            run=&target->runs[target->run_count];
            status=effect_body_run_seed(work,run,op,i);
            if (status!=XR_XIR_OK) return status;
        } else if (run->count==1) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
                uint32_t delta=effect_body_source_word(op,w)-effect_body_source_word(&run->base,w);
                if (delta) {
                    if (!xir_compile_work(work,sizeof(delta))) return XR_XIR_BUDGET;
                    run->delta[w]=delta;run->mask|=UINT32_C(1)<<w;
                }
            }
            ++run->count;
        } else {
            if (run->count==2) {
                if (!xir_compile_work(work,sizeof(EffectBodyRead))) return XR_XIR_BUDGET;
                EffectBodyRead read=effect_body_compile_read(work);
                XrXirStatus status=effect_body_single_column(&read,run->mask,&column);
                if (status!=XR_XIR_OK) return status;
            }
            if (column<=EFFECT_BODY_WORDS) {
                for (uint32_t w=0;w<column;++w) {
                    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
                    if (effect_body_source_word(op,w)!=effect_body_source_word(&run->base,w)) goto new_run;
                }
                if (column<EFFECT_BODY_WORDS) {
                    if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
                    uint32_t expected=effect_body_source_word(&run->base,column)+run->delta[column]*run->count;
                    if (effect_body_source_word(op,column)!=expected) goto new_run;
                }
                for (uint32_t w=column+1;w<EFFECT_BODY_WORDS;++w) {
                    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
                    if (effect_body_source_word(op,w)!=effect_body_source_word(&run->base,w)) goto new_run;
                }
            } else {
                for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                    bool varying=!!(run->mask&(UINT32_C(1)<<w));
                    if (!xir_compile_work(work,2+(varying?2:0))) return XR_XIR_BUDGET;
                    uint32_t expected=effect_body_source_word(&run->base,w);
                    if (varying) expected+=run->delta[w]*run->count;
                    if (effect_body_source_word(op,w)!=expected) goto new_run;
                }
            }
            if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            ++run->count;continue;
        new_run:
            if (!(run->mask&UINT32_C(1))) {
                if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
                if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
            }
            XrXirStatus status=effect_body_run_submit(work,source,target,run,sink);
            if (status==XR_XIR_OK) status=effect_body_runs_reserve(work,target);
            if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(run))) status=XR_XIR_BUDGET;
            if (status==XR_XIR_OK) { run=&target->runs[target->run_count];
                status=effect_body_run_seed(work,run,op,i); }
            if (status!=XR_XIR_OK) return status;
        }
    }
    XrXirStatus status=effect_body_run_submit(work,source,target,run,sink);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,sizeof(target->run_capacity))) return XR_XIR_BUDGET;
    /* Only initialized records are readable after final publication. The
     * resource allocation retains its actual larger charged byte size. */
    target->run_capacity=target->run_count;return XR_XIR_OK;
}

/* Literal codec construction does not publish a semantic generation. */
static inline XrXirStatus effect_body_records_copy(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyFunction *target) {
    return effect_body_records_write(work,source,target,NULL);
}

/* The same bounded reader serves compile and runtime ledger callbacks. A
 * local run reference cannot outlive this request or acquire body permission. */
static XrXirStatus effect_body_run_ref(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t index,uint32_t first,const EffectBodyRun **output) {
    if (!read || !output || !function || !function->runs ||
        function->run_count>function->run_capacity || function->run_count>function->instruction_count ||
        index>=function->run_count || first>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,7)) return XR_XIR_BUDGET;
    const EffectBodyRun *run=&function->runs[index];
    if (run->first!=first || !run->count || run->count>function->instruction_count-first ||
        run->mask&~EFFECT_BODY_MASK || (run->count==1 && run->mask)) return XR_XIR_BAD_STRUCTURE;
    /* A selected physical delta must be nonzero. The array is full length,
     * so there is no packed-column directory or rank to reconstruct. */
    for (uint32_t w=0;run->mask && w<EFFECT_BODY_WORDS;++w) {
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        if (!(run->mask&(UINT32_C(1)<<w))) continue;
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        if (!run->delta[w]) return XR_XIR_BAD_STRUCTURE;
    }
    if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=run;return XR_XIR_OK;
}

/* A short directory is checked in full once during this request. Direct
 * selection needs neither a binary search nor repeated checks of the same
 * first/final/neighbor record. No reference survives the immediate decode. */
static XrXirStatus effect_body_small_record_ref(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,const EffectBodyRun **output) {
    if (!function || !output || !function->run_count || function->run_count>3 ||
        instruction>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,5)) return XR_XIR_BUDGET;
    const EffectBodyRun *first;
    XrXirStatus status=effect_body_run_ref(read,function,0,0,&first);
    if (status!=XR_XIR_OK) return status;
    if (function->run_count==1) {
        if (first->count!=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,3+sizeof(*output))) return XR_XIR_BUDGET;
        *output=first;return XR_XIR_OK;
    }
    const EffectBodyRun *last;
    uint32_t end=function->run_count-1;
    if (!effect_body_read_work(read,sizeof(end)+2)) return XR_XIR_BUDGET;
    status=effect_body_run_ref(read,function,end,function->runs[end].first,&last);
    if (status!=XR_XIR_OK) return status;
    if (last->first+last->count!=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *middle=first;
    if (function->run_count==3) {
        status=effect_body_run_ref(read,function,1,first->count,&middle);
        if (status!=XR_XIR_OK) return status;
    }
    if (!effect_body_read_work(read,7+2*sizeof(middle)+sizeof(*output))) return XR_XIR_BUDGET;
    if (middle->first+middle->count!=last->first) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *ref=instruction<first->count?first:instruction<last->first?middle:last;
    if (instruction<ref->first || instruction-ref->first>=ref->count) return XR_XIR_BAD_STRUCTURE;
    *output=ref;return XR_XIR_OK;
}

/* Random projection uses the actual ordinal directory. Its complete owner
 * admission still walks every run; this bounded read independently checks the
 * first, final and selected adjacent boundaries and never grants semantics. */
static XrXirStatus effect_body_record_ref(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,const EffectBodyRun **output) {
    if (!read || !output || !function || !function->runs ||
        instruction>=function->instruction_count || !function->run_count ||
        function->run_count>function->run_capacity ||
        function->run_count>function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (function->run_count<=3) return effect_body_small_record_ref(read,function,instruction,output);
    if (!effect_body_read_work(read,4*sizeof(uint32_t)+2)) return XR_XIR_BUDGET;
    uint32_t low=1,high=function->run_count-1,first=0,last=high;
    const EffectBodyRun *head,*tail;
    XrXirStatus status=effect_body_run_ref(read,function,0,0,&head);
    if (status!=XR_XIR_OK) return status;
    status=effect_body_run_ref(read,function,last,function->runs[last].first,&tail);
    if (status!=XR_XIR_OK) return status;
    if (tail->first+tail->count!=function->instruction_count || tail->first<=head->count)
        return XR_XIR_BAD_STRUCTURE;
    /* These current endpoints already supply the selected record. Their
     * actual neighboring boundary still needs its own complete shape check. */
    if (instruction<head->count) {
        const EffectBodyRun *next;
        status=effect_body_run_ref(read,function,1,head->count,&next);
        if (status!=XR_XIR_OK) return status;
        if (!effect_body_read_work(read,3+sizeof(*output))) return XR_XIR_BUDGET;
        *output=head;return XR_XIR_OK;
    }
    if (instruction>=tail->first) {
        const EffectBodyRun *previous;
        status=effect_body_run_ref(read,function,last-1,function->runs[last-1].first,&previous);
        if (status!=XR_XIR_OK) return status;
        if (!effect_body_read_work(read,5+sizeof(*output))) return XR_XIR_BUDGET;
        if (previous->first+previous->count!=tail->first ||
            tail->first+tail->count!=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
        *output=tail;return XR_XIR_OK;
    }
    while (low<high) {
        if (!effect_body_read_work(read,3+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
        uint32_t middle=low+(high-low)/2;
        if (function->runs[middle].first<=instruction) low=middle+1;
        else high=middle;
    }
    if (low<=1) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,1+sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t index=low-1;
    if (index==1) first=head->count;
    else {
        const EffectBodyRun *previous;
        status=effect_body_run_ref(read,function,index-1,function->runs[index-1].first,&previous);
        if (status!=XR_XIR_OK) return status;
        first=previous->first+previous->count;
    }
    if (!effect_body_read_work(read,2+sizeof(first))) return XR_XIR_BUDGET;
    const EffectBodyRun *ref;
    status=effect_body_run_ref(read,function,index,first,&ref);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(read,3)) return XR_XIR_BUDGET;
    if (instruction<ref->first || instruction-ref->first>=ref->count) return XR_XIR_BAD_STRUCTURE;
    if (index+1==last) {
        if (ref->first+ref->count!=tail->first) return XR_XIR_BAD_STRUCTURE;
    } else {
        const EffectBodyRun *next;
        status=effect_body_run_ref(read,function,index+1,ref->first+ref->count,&next);
        if (status!=XR_XIR_OK) return status;
    }
    if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=ref;return XR_XIR_OK;
}

/* Only the current sequential scan owns this physical position. Every read
 * rechecks the current canonical run and its neighbor; no decoded row or
 * semantic fact survives a function, block, restart or public request. */
typedef struct EffectBodyRecordCursor {
    uint32_t first,run,last;
} EffectBodyRecordCursor;


/* One complete walk visits every canonical run and its adjacent boundary.
 * Numeric positions are stack-local; the reference is consumed immediately
 * by the existing word decoder. This is a storage projection, not admission. */
static XrXirStatus effect_body_range_ref(const EffectBodyRead *read,
    const EffectBodyFunction *function,EffectBodyRecordCursor *cursor,const EffectBodyRun **output) {
    if (!read || !function || !cursor || !output ||
        !function->run_count || function->run_count>function->instruction_count ||
        cursor->run>=function->run_count || cursor->first>=function->instruction_count ||
        (!cursor->run && cursor->first)) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,5)) return XR_XIR_BUDGET;
    const EffectBodyRun *ref;
    XrXirStatus status=effect_body_run_ref(read,function,cursor->run,cursor->first,&ref);
    if (status!=XR_XIR_OK) return status;
    if (cursor->run+1==function->run_count) {
        if (ref->first+ref->count!=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    } else {
        const EffectBodyRun *next;
        status=effect_body_run_ref(read,function,cursor->run+1,ref->first+ref->count,&next);
        if (status!=XR_XIR_OK) return status;
    }
    if (!effect_body_read_work(read,sizeof(*output)+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
    cursor->first=ref->first+ref->count;++cursor->run;*output=ref;return XR_XIR_OK;
}

static XrXirStatus effect_body_record_cursor_ref(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,EffectBodyRecordCursor *cursor,
    const EffectBodyRun **output) {
    if (!read || !output || !function || !cursor || instruction>=function->instruction_count ||
        !function->run_count || function->run_count>function->instruction_count ||
        cursor->run>=function->run_count || cursor->first>instruction ||
        (cursor->last!=UINT32_MAX && instruction<cursor->last) ||
        (cursor->last==UINT32_MAX && (cursor->run || cursor->first))) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,4)) return XR_XIR_BUDGET;
    while (cursor->run<function->run_count) {
        const EffectBodyRun *ref;
        XrXirStatus status=effect_body_run_ref(read,function,cursor->run,cursor->first,&ref);
        if (status!=XR_XIR_OK) return status;
        if (instruction-cursor->first<ref->count) {
            if (cursor->run+1==function->run_count) {
                if (cursor->first+ref->count!=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
            } else {
                const EffectBodyRun *next;
                status=effect_body_run_ref(read,function,cursor->run+1,cursor->first+ref->count,&next);
                if (status!=XR_XIR_OK) return status;
            }
            if (!effect_body_read_work(read,sizeof(*output)+sizeof(cursor->last))) return XR_XIR_BUDGET;
            cursor->last=instruction;*output=ref;return XR_XIR_OK;
        }
        if (!effect_body_read_work(read,2*sizeof(uint32_t))) return XR_XIR_BUDGET;
        cursor->first+=ref->count;++cursor->run;
    }
    return XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_body_run_word(const EffectBodyRead *read,const EffectBodyRun *ref,
    uint32_t instruction,uint32_t word,uint32_t *output) {
    if (!ref || !output || word>=EFFECT_BODY_WORDS || instruction<ref->first ||
        instruction-ref->first>=ref->count) return XR_XIR_BAD_STRUCTURE;
    bool delta=!!(ref->mask&(UINT32_C(1)<<word));
    if (!effect_body_read_work(read,1+(delta?3:0)+sizeof(*output))) return XR_XIR_BUDGET;
    *output=effect_body_source_word(&ref->base,word)+(delta?ref->delta[word]*(instruction-ref->first):0);
    return XR_XIR_OK;
}

typedef struct EffectBodyInstructionFields {
    XrXirOp op;
    XrXirType type;
    uint32_t source;
} EffectBodyInstructionFields;

/* Both projections read every physical word through the same canonical
 * run, neighbor and range checks. Only the requested output is published. */
static XrXirStatus effect_body_instruction_ref_words(const EffectBodyRead *read,
    const EffectBodyRun *ref,uint32_t instruction,uint32_t *words) {
    if (!ref || !words || instruction<ref->first || instruction-ref->first>=ref->count)
        return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,sizeof(uint32_t))) return XR_XIR_BUDGET;

    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        bool present=!!(ref->mask&(UINT32_C(1)<<w));
        if (!effect_body_read_work(read,1+(present?4:0)+sizeof(words[w]))) return XR_XIR_BUDGET;
        words[w]=effect_body_source_word(&ref->base,w)+(present?ref->delta[w]*(instruction-ref->first):0);
    }
    if (words[0]<=XR_XIR_INVALID || words[0]>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static XrXirStatus effect_body_instruction_words_get(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,uint32_t *words) {
    if (!words) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *ref;
    XrXirStatus status=effect_body_record_ref(read,function,instruction,&ref);
    return status==XR_XIR_OK?effect_body_instruction_ref_words(read,ref,instruction,words):status;
}

/* The canonical run has already bounded every selected column. Physical
 * field assignments consume only their own mask-selected delta slots, including
 * unused fields and both unsigned halves of the signed immediate. */
static XrXirInstruction effect_body_instruction_words_value(const XrXirInstruction *base,const uint32_t *words,
    uint32_t mask,const uint32_t *delta,uint32_t offset) {
    XrXirInstruction value;
    value.op=(XrXirOp)((base?effect_body_source_word(base,0):words[0])+((mask&UINT32_C(1))?delta[0]*offset:0));
    value.type=(base?effect_body_source_word(base,1):words[1])+((mask&UINT32_C(2))?delta[1]*offset:0);
    value.args[0]=(base?effect_body_source_word(base,2):words[2])+((mask&UINT32_C(4))?delta[2]*offset:0);
    value.args[1]=(base?effect_body_source_word(base,3):words[3])+((mask&UINT32_C(8))?delta[3]*offset:0);
    value.targets[0]=(base?effect_body_source_word(base,4):words[4])+((mask&UINT32_C(16))?delta[4]*offset:0);
    value.targets[1]=(base?effect_body_source_word(base,5):words[5])+((mask&UINT32_C(32))?delta[5]*offset:0);
    uint64_t immediate=(base?effect_body_source_word(base,6):words[6])+((mask&UINT32_C(64))?delta[6]*offset:0);
    immediate|=(uint64_t)((base?effect_body_source_word(base,7):words[7])+((mask&UINT32_C(128))?delta[7]*offset:0))<<32;
    memcpy(&value.immediate,&immediate,sizeof(immediate));
    value.type_arguments[0]=(base?effect_body_source_word(base,8):words[8])+((mask&UINT32_C(256))?delta[8]*offset:0);
    value.type_arguments[1]=(base?effect_body_source_word(base,9):words[9])+((mask&UINT32_C(512))?delta[9]*offset:0);
    return value;
}

/* Counting and opcode validation finish before the full row is published.
 * The value core writes no temporary ten-word array; its actual cursor,
 * selected reads and complete Instruction output remain charged. */
static XrXirStatus effect_body_instruction_ref_prepare(const EffectBodyRead *read,
    const EffectBodyRun *ref,uint32_t instruction) {
    if (!read || !ref || (ref->mask&~EFFECT_BODY_MASK) || (ref->count==1 && ref->mask) ||
        instruction<ref->first || instruction-ref->first>=ref->count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,1+sizeof(uint32_t)+((ref->mask&UINT32_C(1))?3:0)))
        return XR_XIR_BUDGET;
    uint32_t operation=effect_body_source_word(&ref->base,0)+((ref->mask&UINT32_C(1))?
        ref->delta[0]*(instruction-ref->first):0);
    if (operation<=XR_XIR_INVALID || operation>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    uint64_t units=EFFECT_BODY_WORDS+sizeof(XrXirInstruction)+2;
    for (uint32_t w=0;ref->mask && w<EFFECT_BODY_WORDS;++w) {
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        if (ref->mask&(UINT32_C(1)<<w)) {
            if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
            if (!ref->delta[w]) return XR_XIR_BAD_STRUCTURE;
            units+=4;
        }
    }
    if (!effect_body_read_work(read,units+EFFECT_BODY_WORDS)) return XR_XIR_BUDGET;
    return XR_XIR_OK;
}
static XrXirStatus effect_body_instruction_ref_value(const EffectBodyRead *read,
    const EffectBodyRun *ref,uint32_t instruction,XrXirInstruction *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_instruction_ref_prepare(read,ref,instruction);
    if (status!=XR_XIR_OK) return status;
    *output=effect_body_instruction_words_value(&ref->base,NULL,ref->mask,ref->delta,instruction-ref->first);
    return XR_XIR_OK;
}

static inline XrXirStatus effect_body_instruction_words_publish(const EffectBodyRead *read,
    const uint32_t *words,XrXirInstruction *output) {
    if (!words || !output) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,sizeof(uint32_t)+2*EFFECT_BODY_WORDS+sizeof(*output)+2))
        return XR_XIR_BUDGET;
    *output=effect_body_instruction_words_value(NULL,words,0,NULL,0);return XR_XIR_OK;
}

static XrXirStatus effect_body_instruction_get(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,XrXirInstruction *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *ref;
    XrXirStatus status=effect_body_record_ref(read,function,instruction,&ref);
    return status==XR_XIR_OK?effect_body_instruction_ref_value(read,ref,instruction,output):status;
}

static XrXirStatus effect_body_instruction_fields_get(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,EffectBodyInstructionFields *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    uint32_t words[EFFECT_BODY_WORDS];
    XrXirStatus status=effect_body_instruction_words_get(read,function,instruction,words);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(read,sizeof(*output)+2)) return XR_XIR_BUDGET;
    *output=(EffectBodyInstructionFields){(XrXirOp)words[0],(XrXirType)words[1],words[2]};
    return XR_XIR_OK;
}

static XrXirStatus effect_body_instruction_cursor_words(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,EffectBodyRecordCursor *cursor,uint32_t *words) {
    if (!words) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *ref;
    XrXirStatus status=effect_body_record_cursor_ref(read,function,instruction,cursor,&ref);
    return status==XR_XIR_OK?effect_body_instruction_ref_words(read,ref,instruction,words):status;
}

static inline XrXirStatus effect_body_instruction_cursor_fields(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,EffectBodyRecordCursor *cursor,
    EffectBodyInstructionFields *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    uint32_t words[EFFECT_BODY_WORDS];
    XrXirStatus status=effect_body_instruction_cursor_words(read,function,instruction,cursor,words);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(read,sizeof(*output)+2)) return XR_XIR_BUDGET;
    *output=(EffectBodyInstructionFields){(XrXirOp)words[0],(XrXirType)words[1],words[2]};
    return XR_XIR_OK;
}

static inline XrXirStatus effect_body_instruction_cursor_get(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,EffectBodyRecordCursor *cursor,
    XrXirInstruction *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *ref;
    XrXirStatus status=effect_body_record_cursor_ref(read,function,instruction,cursor,&ref);
    return status==XR_XIR_OK?effect_body_instruction_ref_value(read,ref,instruction,output):status;
}

static XrXirStatus effect_body_operand_type(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t value,XrXirType *output) {
    if (!function || !output || value>=(uint64_t)function->parameter_count+function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    if (value<function->parameter_count) {
        if (!function->parameters) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,1+sizeof(*output))) return XR_XIR_BUDGET;
        *output=function->parameters[value];return XR_XIR_OK;
    }
    const EffectBodyRun *ref;uint32_t type=0;
    XrXirStatus status=effect_body_record_ref(read,function,value-function->parameter_count,&ref);
    if (status==XR_XIR_OK) status=effect_body_run_word(read,ref,value-function->parameter_count,1,&type);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(XrXirType)type;return XR_XIR_OK;
}

static XrXirStatus effect_body_instruction_op(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,XrXirOp *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *ref;
    if (!effect_body_read_work(read,sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t opcode=XR_XIR_INVALID;
    XrXirStatus status=effect_body_record_ref(read,function,instruction,&ref);
    if (status==XR_XIR_OK) status=effect_body_run_word(read,ref,instruction,0,&opcode);
    if (status!=XR_XIR_OK) return status;
    if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(XrXirOp)opcode;return XR_XIR_OK;
}

/* This projection validates the same canonical run and publishes only the
 * two actual fields requested by a classifier. It never builds a full row. */
static XrXirStatus effect_body_instruction_shape_words(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,uint32_t *opcode,uint32_t *type) {
    if (!opcode || !type) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *ref;uint32_t operation=XR_XIR_INVALID,physical_type=XR_XIR_UNIT;
    if (!effect_body_read_work(read,2*sizeof(uint32_t))) return XR_XIR_BUDGET;
    XrXirStatus status=effect_body_record_ref(read,function,instruction,&ref);
    if (status==XR_XIR_OK) status=effect_body_run_word(read,ref,instruction,0,&operation);
    if (status==XR_XIR_OK) status=effect_body_run_word(read,ref,instruction,1,&physical_type);
    if (status!=XR_XIR_OK) return status;
    if (operation<=XR_XIR_INVALID || operation>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,2*sizeof(*opcode))) return XR_XIR_BUDGET;
    *opcode=operation;*type=physical_type;return XR_XIR_OK;
}

/* The canonical reference was obtained in this same correspondence call.
 * Constant columns avoid a per-row mask interpretation; the changing column
 * retains unsigned multiply/add. Signed and unused bits are never omitted. */
static XrXirStatus effect_body_column_match(const EffectBodyRead *read,
    const EffectBodyRun *ref,const XrXirFunction *source,uint32_t column,bool *same) {
    if (!ref || !source || column>EFFECT_BODY_WORDS ||
        ref->mask!=(column<EFFECT_BODY_WORDS?(UINT32_C(1)<<column):0) ||
        !ref->count || ref->first>=source->instruction_count ||
        ref->count>source->instruction_count-ref->first || !source->instructions ||
        (column<EFFECT_BODY_WORDS && !ref->delta[column])) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,6)) return XR_XIR_BUDGET;
    for (uint32_t i=ref->first;i-ref->first<ref->count;++i) {
        for (uint32_t w=0;w<column;++w) {
            if (!effect_body_read_work(read,2)) return XR_XIR_BUDGET;
            if (effect_body_source_word(&ref->base,w)!=effect_body_source_word(&source->instructions[i],w)) {
                if (!same) return XR_XIR_BAD_STRUCTURE;
                if (!effect_body_read_work(read,sizeof(*same))) return XR_XIR_BUDGET;
                *same=false;
            }
        }
        if (column<EFFECT_BODY_WORDS) {
            if (!effect_body_read_work(read,6)) return XR_XIR_BUDGET;
            if (effect_body_source_word(&ref->base,column)+ref->delta[column]*(i-ref->first)!=
                effect_body_source_word(&source->instructions[i],column)) {
                if (!same) return XR_XIR_BAD_STRUCTURE;
                if (!effect_body_read_work(read,sizeof(*same))) return XR_XIR_BUDGET;
                *same=false;
            }
        }
        for (uint32_t w=column+1;w<EFFECT_BODY_WORDS;++w) {
            if (!effect_body_read_work(read,2)) return XR_XIR_BUDGET;
            if (effect_body_source_word(&ref->base,w)!=effect_body_source_word(&source->instructions[i],w)) {
                if (!same) return XR_XIR_BAD_STRUCTURE;
                if (!effect_body_read_work(read,sizeof(*same))) return XR_XIR_BUDGET;
                *same=false;
            }
        }
    }
    return XR_XIR_OK;
}

/* Independent correspondence is linear in physical rows and fields. It
 * validates every run boundary and all words, even unused instruction words. */
static XrXirStatus effect_body_records_compare(const EffectBodyRead *read,
    const EffectBodyFunction *function,const XrXirFunction *source,bool *same) {
    if (!function || !source || (source->instruction_count && !source->instructions) ||
        function->run_count>function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (function->instruction_count!=source->instruction_count) {
        if (!same) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,sizeof(*same))) return XR_XIR_BUDGET;
        *same=false;return XR_XIR_OK;
    }
    uint32_t first=0;
    for (uint32_t r=0;r<function->run_count;++r) {
        const EffectBodyRun *ref;
        XrXirStatus status=effect_body_run_ref(read,function,r,first,&ref);
        if (status!=XR_XIR_OK) return status;
        uint32_t column=UINT32_MAX;
        if (!effect_body_read_work(read,sizeof(column)+2)) return XR_XIR_BUDGET;
        if (ref->count>2) {
            status=effect_body_single_column(read,ref->mask,&column);
            if (status!=XR_XIR_OK) return status;
        }
        if (column<=EFFECT_BODY_WORDS) {
            status=effect_body_column_match(read,ref,source,column,same);
            if (status!=XR_XIR_OK) return status;
        } else for (uint32_t i=first;i-first<ref->count;++i) {
            for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                bool delta=!!(ref->mask&(UINT32_C(1)<<w));
                if (!effect_body_read_work(read,3+(delta?4:0))) return XR_XIR_BUDGET;
                if (effect_body_source_word(&ref->base,w)+(delta?ref->delta[w]*(i-first):0)!=
                    effect_body_source_word(&source->instructions[i],w)) {
                    if (!same) return XR_XIR_BAD_STRUCTURE;
                    if (!effect_body_read_work(read,sizeof(*same))) return XR_XIR_BUDGET;
                    *same=false;
                }
            }
        }
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        first+=ref->count;
    }
    return first==function->instruction_count?XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_body_records_match(const EffectBodyRead *read,
    const EffectBodyFunction *function,const XrXirFunction *source) {
    return effect_body_records_compare(read,function,source,NULL);
}
