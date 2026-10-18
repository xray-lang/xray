/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_records.inc.c - Lossless literal pairs and affine instruction runs
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


static bool effect_body_function_storage_shape(const EffectBodyFunction *body) {
    if (!body) return false;
    if (body->literals) return body->instruction_count && !body->runs &&
        body->run_count==body->instruction_count && body->run_capacity==body->instruction_count;
    return body->run_count<=body->run_capacity && body->run_capacity<=body->instruction_count &&
        !!body->runs==!!body->run_capacity && !!body->run_count==!!body->instruction_count;
}
static XrXirStatus effect_body_literal_get(const EffectBodyRead *read,
    const EffectBodyFunction *body,uint32_t i,XrXirInstruction *output) {
    if (!read || !output || !effect_body_function_storage_shape(body) || !body->literals ||
        i>=body->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,4+sizeof(*output))) return XR_XIR_BUDGET;
    const XrXirInstruction *op=&body->literals[i];
    if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    *output=*op;return XR_XIR_OK;
}
static XrXirStatus effect_body_literal_words(const EffectBodyRead *read,
    const EffectBodyFunction *body,uint32_t i,uint32_t *words) {
    if (!words) return XR_XIR_BAD_STRUCTURE;
    XrXirInstruction value;XrXirStatus status=effect_body_literal_get(read,body,i,&value);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(read,EFFECT_BODY_WORDS*(1+sizeof(*words)))) return XR_XIR_BUDGET;
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) words[w]=effect_body_source_word(&value,w);
    return XR_XIR_OK;
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
    if (run->count==2 && run->mask!=EFFECT_BODY_MASK) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t w=0;run->count!=2 && w<EFFECT_BODY_WORDS;++w) {
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
    if (count==2) {
        if (mask!=EFFECT_BODY_MASK) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(work,8+sizeof(mask))) return XR_XIR_BUDGET;
        mask=(uint32_t)((uint32_t)run->base.op!=run->delta[0]) |
            ((uint32_t)((uint32_t)run->base.type!=run->delta[1])<<1);
    }
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

#include "xxir_effect_snapshot_pairs.inc.c"

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
            if (!xir_compile_work(work,2*sizeof(uint32_t)+EFFECT_BODY_WORDS*(1+sizeof(uint32_t)))) return XR_XIR_BUDGET;
            for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) run->delta[w]=effect_body_source_word(op,w);
            run->mask=EFFECT_BODY_MASK;run->count=2;
        } else {
            if (run->count==2) {
                bool extended=false;
                XrXirStatus status=effect_body_pair_extend(work,run,op,&extended);
                if (status!=XR_XIR_OK) return status;
                if (!extended) goto new_run;
                if (!xir_compile_work(work,sizeof(EffectBodyRead))) return XR_XIR_BUDGET;
                EffectBodyRead read=effect_body_compile_read(work);
                status=effect_body_single_column(&read,run->mask,&column);
                if (status!=XR_XIR_OK) return status;
                continue;
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

/* Five actual physical slots have only 32 selection patterns. Direct
 * dispatch retains every selected nonzero check in ascending field order. */
static XrXirStatus effect_body_delta_group(const EffectBodyRead *read,uint32_t mask,
    const uint32_t *delta,uint64_t *units) {
    if (!mask) return XR_XIR_OK;
    if (mask>31 || !delta) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,3)) return XR_XIR_BUDGET;
#define EFFECT_BODY_DELTA_CHECK(I) do { \
    if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET; \
    if (!delta[I]) return XR_XIR_BAD_STRUCTURE; \
} while (0)
    switch (mask) {
    case 1:EFFECT_BODY_DELTA_CHECK(0);if (units) *units+=4;break;
    case 2:EFFECT_BODY_DELTA_CHECK(1);if (units) *units+=4;break;
    case 3:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(1);if (units) *units+=8;break;
    case 4:EFFECT_BODY_DELTA_CHECK(2);if (units) *units+=4;break;
    case 5:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(2);if (units) *units+=8;break;
    case 6:EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(2);if (units) *units+=8;break;
    case 7:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(2);if (units) *units+=12;break;
    case 8:EFFECT_BODY_DELTA_CHECK(3);if (units) *units+=4;break;
    case 9:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(3);if (units) *units+=8;break;
    case 10:EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(3);if (units) *units+=8;break;
    case 11:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(3);if (units) *units+=12;break;
    case 12:EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(3);if (units) *units+=8;break;
    case 13:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(3);if (units) *units+=12;break;
    case 14:EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(3);if (units) *units+=12;break;
    case 15:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(3);if (units) *units+=16;break;
    case 16:EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=4;break;
    case 17:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=8;break;
    case 18:EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=8;break;
    case 19:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=12;break;
    case 20:EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=8;break;
    case 21:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=12;break;
    case 22:EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=12;break;
    case 23:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=16;break;
    case 24:EFFECT_BODY_DELTA_CHECK(3);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=8;break;
    case 25:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(3);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=12;break;
    case 26:EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(3);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=12;break;
    case 27:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(3);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=16;break;
    case 28:EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(3);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=12;break;
    case 29:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(3);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=16;break;
    case 30:EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(3);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=16;break;
    case 31:EFFECT_BODY_DELTA_CHECK(0);EFFECT_BODY_DELTA_CHECK(1);EFFECT_BODY_DELTA_CHECK(2);EFFECT_BODY_DELTA_CHECK(3);EFFECT_BODY_DELTA_CHECK(4);if (units) *units+=20;break;
    default:return XR_XIR_BAD_STRUCTURE;
    }
#undef EFFECT_BODY_DELTA_CHECK
    return XR_XIR_OK;
}

/* The same bounded reader serves compile and runtime ledger callbacks. A
 * local run reference cannot outlive this request or acquire body permission. */
static XrXirStatus effect_body_run_ref(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t index,uint32_t first,const EffectBodyRun **output) {
    if (!read || !output || !function || !function->runs ||
        function->run_count>function->run_capacity || function->run_count>function->instruction_count ||
        index>=function->run_count || first>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,8)) return XR_XIR_BUDGET;
    const EffectBodyRun *run=&function->runs[index];
    if (run->first!=first || !run->count || run->count>function->instruction_count-first ||
        run->mask&~EFFECT_BODY_MASK || (run->count==1 && run->mask)) return XR_XIR_BAD_STRUCTURE;
    /* The full mask is the sole two-row tag, whose literal words may be zero.
     * Longer runs require nonzero selected differences. No rank directory or
     * remembered validation is needed by either physical representation. */
    if (run->count==2) {
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        if (run->mask!=EFFECT_BODY_MASK) return XR_XIR_BAD_STRUCTURE;
    }
    else if (run->mask) {
        if (!effect_body_read_work(read,2)) return XR_XIR_BUDGET;
        XrXirStatus status=effect_body_delta_group(read,run->mask&31,run->delta,NULL);
        if (status==XR_XIR_OK) status=effect_body_delta_group(read,run->mask>>5,run->delta+5,NULL);
        if (status!=XR_XIR_OK) return status;
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
    if (!effect_body_read_work(read,2)) return XR_XIR_BUDGET;
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

/* A literal range borrows this caller's stack descriptor until its next
 * request. Inactive deltas are never initialized, copied or inspected. */
static XrXirStatus effect_body_range_take(const EffectBodyRead *read,
    const EffectBodyFunction *function,EffectBodyRecordCursor *cursor,const EffectBodyRun **output,EffectBodyRun *storage) {
    if (function && function->literals) {
        if (!read || !output || !storage || !cursor || !effect_body_function_storage_shape(function) ||
            cursor->run!=cursor->first || cursor->first>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
        uint32_t i=cursor->first;
        if (!effect_body_read_work(read,5+3*sizeof(uint32_t)+sizeof(storage->base)+sizeof(*output))) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&function->literals[i];
        if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        storage->first=i;storage->count=1;storage->mask=0;storage->base=*op;
        ++cursor->first;++cursor->run;*output=storage;return XR_XIR_OK;
    }
    return effect_body_range_ref(read,function,cursor,output);
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
    if (ref->count==2) {
        if (ref->mask!=EFFECT_BODY_MASK) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,2+sizeof(*output))) return XR_XIR_BUDGET;
        *output=instruction==ref->first?effect_body_source_word(&ref->base,word):ref->delta[word];
        return XR_XIR_OK;
    }
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
        if (ref->count==2) {
            XrXirStatus status=effect_body_run_word(read,ref,instruction,w,&words[w]);
            if (status!=XR_XIR_OK) return status;
            continue;
        }
        bool present=!!(ref->mask&(UINT32_C(1)<<w));
        if (!effect_body_read_work(read,1+(present?4:0)+sizeof(words[w]))) return XR_XIR_BUDGET;
        words[w]=effect_body_source_word(&ref->base,w)+(present?ref->delta[w]*(instruction-ref->first):0);
    }
    if (words[0]<=XR_XIR_INVALID || words[0]>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static XrXirStatus effect_body_instruction_words_get(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,uint32_t *words) {
    if (function && function->literals) return effect_body_literal_words(read,function,instruction,words);

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
    if (ref->count==2) {
        if (ref->mask!=EFFECT_BODY_MASK) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,2+sizeof(uint32_t))) return XR_XIR_BUDGET;
        uint32_t opcode=instruction==ref->first?(uint32_t)ref->base.op:ref->delta[0];
        if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        return effect_body_read_work(read,sizeof(XrXirInstruction)+EFFECT_BODY_WORDS+2)?XR_XIR_OK:XR_XIR_BUDGET;
    }
    if (!effect_body_read_work(read,1+sizeof(uint32_t)+((ref->mask&UINT32_C(1))?3:0)))
        return XR_XIR_BUDGET;
    uint32_t operation=effect_body_source_word(&ref->base,0)+((ref->mask&UINT32_C(1))?
        ref->delta[0]*(instruction-ref->first):0);
    if (operation<=XR_XIR_INVALID || operation>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    uint64_t units=EFFECT_BODY_WORDS+sizeof(XrXirInstruction)+2;
    if (ref->mask) {
        if (!effect_body_read_work(read,2)) return XR_XIR_BUDGET;
        XrXirStatus status=effect_body_delta_group(read,ref->mask&31,ref->delta,&units);
        if (status==XR_XIR_OK) status=effect_body_delta_group(read,ref->mask>>5,ref->delta+5,&units);
        if (status!=XR_XIR_OK) return status;
    }
    if (!effect_body_read_work(read,units+EFFECT_BODY_WORDS)) return XR_XIR_BUDGET;
    return XR_XIR_OK;
}
static XrXirStatus effect_body_instruction_ref_value(const EffectBodyRead *read,
    const EffectBodyRun *ref,uint32_t instruction,XrXirInstruction *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_instruction_ref_prepare(read,ref,instruction);
    if (status!=XR_XIR_OK) return status;
    if (ref->count==2) *output=instruction==ref->first?ref->base:
        effect_body_instruction_words_value(NULL,ref->delta,0,NULL,0);
    else *output=effect_body_instruction_words_value(&ref->base,NULL,ref->mask,ref->delta,instruction-ref->first);
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
    if (function && function->literals) return effect_body_literal_get(read,function,instruction,output);

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

    if (function && function->literals) {
        if (!cursor || cursor->first>instruction || (cursor->last!=UINT32_MAX && instruction<cursor->last) ||
            (cursor->last==UINT32_MAX && (cursor->run || cursor->first))) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,4+3*sizeof(uint32_t))) return XR_XIR_BUDGET;
        XrXirStatus status=effect_body_literal_words(read,function,instruction,words);
        if (status==XR_XIR_OK) {cursor->first=instruction;cursor->run=instruction;cursor->last=instruction;}
        return status;
    }

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

    if (function && function->literals) {
        if (!cursor || cursor->first>instruction || (cursor->last!=UINT32_MAX && instruction<cursor->last) ||
            (cursor->last==UINT32_MAX && (cursor->run || cursor->first))) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,4+3*sizeof(uint32_t))) return XR_XIR_BUDGET;
        XrXirStatus status=effect_body_literal_get(read,function,instruction,output);
        if (status==XR_XIR_OK) {cursor->first=instruction;cursor->run=instruction;cursor->last=instruction;}
        return status;
    }

    if (!output) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *ref;
    XrXirStatus status=effect_body_record_cursor_ref(read,function,instruction,cursor,&ref);
    return status==XR_XIR_OK?effect_body_instruction_ref_value(read,ref,instruction,output):status;
}

static XrXirStatus effect_body_operand_type(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t value,XrXirType *output) {

    if (function && function->literals && value>=function->parameter_count) {
        if (!output || !effect_body_function_storage_shape(function) ||
            value-function->parameter_count>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,4+sizeof(*output)+sizeof(uint32_t))) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&function->literals[value-function->parameter_count];
        if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        *output=op->type;return XR_XIR_OK;
    }

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

    if (function && function->literals) {
        if (!output || !effect_body_function_storage_shape(function) || instruction>=function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,4+sizeof(*output))) return XR_XIR_BUDGET;
        XrXirOp opcode=function->literals[instruction].op;
        if (opcode<=XR_XIR_INVALID || opcode>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        *output=opcode;return XR_XIR_OK;
    }

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

    if (function && function->literals) {
        if (!opcode || !type || !effect_body_function_storage_shape(function) || instruction>=function->instruction_count)
            return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,4+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
        const XrXirInstruction *op=&function->literals[instruction];
        if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        *opcode=(uint32_t)op->op;*type=op->type;return XR_XIR_OK;
    }

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

    if (function && function->literals) {
        if (!source || !effect_body_function_storage_shape(function) ||
            function->instruction_count!=source->instruction_count || !source->instructions) return XR_XIR_BAD_STRUCTURE;
        for (uint32_t i=0;i<function->instruction_count;++i) {
            if (!effect_body_read_work(read,3+sizeof(uint32_t))) return XR_XIR_BUDGET;
            if (function->literals[i].op<=XR_XIR_INVALID || function->literals[i].op>=XR_XIR_OP_COUNT)
                return XR_XIR_BAD_STRUCTURE;
            for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                if (!effect_body_read_work(read,2)) return XR_XIR_BUDGET;
                if (effect_body_source_word(&function->literals[i],w)!=effect_body_source_word(&source->instructions[i],w)) {
                    if (!same) return XR_XIR_BAD_STRUCTURE;
                    if (!effect_body_read_work(read,sizeof(*same))) return XR_XIR_BUDGET;
                    *same=false;
                }
            }
        }
        return XR_XIR_OK;
    }

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
        if (ref->count==2) {
            status=effect_body_pair_compare(read,ref,source,same);
            if (status!=XR_XIR_OK) return status;
        } else if (column<=EFFECT_BODY_WORDS) {
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
