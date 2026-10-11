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
static XrXirStatus effect_body_stream_reserve(const XrXirCompileContext *work,
    EffectBodyFunction *function,uint32_t add) {
    if (function->stream_words>function->stream_capacity ||
        add>UINT32_MAX-function->stream_words) return XR_XIR_BAD_STRUCTURE;
    uint32_t needed=function->stream_words+add;
    if (needed<=function->stream_capacity) return XR_XIR_OK;
    uint32_t capacity=function->stream_capacity?function->stream_capacity:64;
    while (capacity<needed) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        capacity=capacity>UINT32_MAX/2?needed:capacity*2;
    }
    if ((uint64_t)capacity>SIZE_MAX/sizeof(uint32_t)) return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    uint32_t *storage=xir_compile_alloc(work,(size_t)capacity*sizeof(*storage),&status);
    if (!storage) return status;
    if (!xir_compile_work(work,(uint64_t)function->stream_words*sizeof(*storage)+3)) {
        xr_compile_resources_free(storage);return XR_XIR_BUDGET;
    }
    if (function->stream_words) memcpy(storage,function->stream,
        (size_t)function->stream_words*sizeof(*storage));
    xr_compile_resources_free(function->stream);function->stream=storage;
    function->stream_capacity=capacity;return XR_XIR_OK;
}

static XrXirStatus effect_body_run_append(const XrXirCompileContext *work,
    EffectBodyFunction *function,const EffectBodyPendingRun *run) {
    uint32_t deltas=0;
    if (!run->count || run->first>=function->instruction_count ||
        run->count>function->instruction_count-run->first || run->mask&~EFFECT_BODY_MASK)
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (!(run->mask&(UINT32_C(1)<<w))) continue;
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (run->count==1 || !run->delta[w]) return XR_XIR_BAD_STRUCTURE;
        ++deltas;
    }
    uint32_t words=EFFECT_BODY_HEADER+deltas;
    XrXirStatus status=effect_body_stream_reserve(work,function,words);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,(uint64_t)words*sizeof(uint32_t)+2)) return XR_XIR_BUDGET;
    uint32_t *out=function->stream+function->stream_words;
    out[0]=run->first;out[1]=run->count;out[2]=run->mask;
    memcpy(out+3,run->base,sizeof(run->base));uint32_t at=EFFECT_BODY_HEADER;
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w)
        if (run->mask&(UINT32_C(1)<<w)) out[at++]=run->delta[w];
    function->stream_words+=words;++function->run_count;return XR_XIR_OK;
}

static XrXirStatus effect_body_run_seed(const XrXirCompileContext *work,
    EffectBodyPendingRun *run,const XrXirInstruction *source,uint32_t first) {
    if (!xir_compile_work(work,3+EFFECT_BODY_WORDS*(1+sizeof(uint32_t)))) return XR_XIR_BUDGET;
    run->first=first;run->count=1;run->mask=0;
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w)
        run->base[w]=effect_body_source_word(source,w);
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
    const XrXirFunction *source,EffectBodyFunction *target,const EffectBodyPendingRun *run,
    const EffectBodyWriteSink *sink) {
    XrXirStatus status=effect_body_run_append(work,target,run);
    if (status!=XR_XIR_OK || !sink) return status;
    if (!sink->owner || !sink->row || !sink->span) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,5)) return XR_XIR_BUDGET;
    if (run->count>1 && !(run->mask&UINT32_C(3))) return sink->span(sink->owner,run->first,run->count);
    for (uint32_t i=run->first;i<run->first+run->count;++i) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        status=sink->row(sink->owner,i,&source->instructions[i]);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* The writer reads each physical row once. It does not materialize a second
 * forty-byte instruction array or perform a separate whole-body copy pass. */
static XrXirStatus effect_body_records_write(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyFunction *target,const EffectBodyWriteSink *sink) {
    if (source->instruction_count && !source->instructions) return XR_XIR_BAD_STRUCTURE;
    /* Only count is read before the first seed. Mask guards every delta read;
     * all ten base words are still copied from each genuine run's first row. */
    if (!xir_compile_work(work,sizeof(uint32_t))) return XR_XIR_BUDGET;
    EffectBodyPendingRun run;run.count=0;
    for (uint32_t i=0;i<source->instruction_count;++i) {
        const XrXirInstruction *op=&source->instructions[i];
        if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
        if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        if (!run.count) {
            XrXirStatus status=effect_body_run_seed(work,&run,op,i);
            if (status!=XR_XIR_OK) return status;
        } else if (run.count==1) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
                uint32_t delta=effect_body_source_word(op,w)-run.base[w];
                if (delta) {
                    if (!xir_compile_work(work,sizeof(delta))) return XR_XIR_BUDGET;
                    run.delta[w]=delta;run.mask|=UINT32_C(1)<<w;
                }
            }
            ++run.count;
        } else {
            bool same=true;
            for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                bool varying=!!(run.mask&(UINT32_C(1)<<w));
                if (!xir_compile_work(work,2+(varying?2:0))) return XR_XIR_BUDGET;
                uint32_t expected=run.base[w];
                if (varying) expected+=run.delta[w]*run.count;
                if (effect_body_source_word(op,w)!=expected) same=false;
            }
            if (same) {
                if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
                ++run.count;
            } else {
                XrXirStatus status=effect_body_run_submit(work,source,target,&run,sink);
                if (status==XR_XIR_OK) status=effect_body_run_seed(work,&run,op,i);
                if (status!=XR_XIR_OK) return status;
            }
        }
    }
    return run.count?effect_body_run_submit(work,source,target,&run,sink):XR_XIR_OK;
}

/* Literal codec construction does not publish a semantic generation. */
static inline XrXirStatus effect_body_records_copy(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyFunction *target) {
    return effect_body_records_write(work,source,target,NULL);
}

/* The same bounded reader serves compile and runtime ledger callbacks. A
 * local run reference cannot outlive this request or acquire body permission. */
static XrXirStatus effect_body_run_ref(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t offset,uint32_t first,EffectBodyRunRef *output) {
    if (!read || !output || !function || function->stream_words>function->stream_capacity ||
        !function->stream || offset>function->stream_words ||
        EFFECT_BODY_HEADER>function->stream_words-offset) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,4)) return XR_XIR_BUDGET;
    const uint32_t *record=function->stream+offset;
    uint32_t count=record[1],mask=record[2],deltas=0;
    if (record[0]!=first || !count || first>=function->instruction_count ||
        count>function->instruction_count-first || mask&~EFFECT_BODY_MASK || (count==1 && mask))
        return XR_XIR_BAD_STRUCTURE;
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        deltas+=!!(mask&(UINT32_C(1)<<w));
    }
    if (deltas>function->stream_words-offset-EFFECT_BODY_HEADER) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t w=0;w<deltas;++w) {
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        if (!record[EFFECT_BODY_HEADER+w]) return XR_XIR_BAD_STRUCTURE;
    }
    if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(EffectBodyRunRef){record+3,record+EFFECT_BODY_HEADER,first,count,mask,
        offset+EFFECT_BODY_HEADER+deltas};return XR_XIR_OK;
}

static XrXirStatus effect_body_record_ref(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,EffectBodyRunRef *output) {
    if (!output || !function || instruction>=function->instruction_count || !function->run_count ||
        function->run_count>function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t offset=0,first=0;
    for (uint32_t r=0;r<function->run_count;++r) {
        EffectBodyRunRef ref;
        XrXirStatus status=effect_body_run_ref(read,function,offset,first,&ref);
        if (status!=XR_XIR_OK) return status;
        if (instruction-first<ref.count) {
            if (r+1==function->run_count) {
                if (ref.next!=function->stream_words || first+ref.count!=function->instruction_count)
                    return XR_XIR_BAD_STRUCTURE;
            } else {
                EffectBodyRunRef next;
                status=effect_body_run_ref(read,function,ref.next,first+ref.count,&next);
                if (status!=XR_XIR_OK) return status;
            }
            if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
            *output=ref;return XR_XIR_OK;
        }
        first+=ref.count;offset=ref.next;
    }
    return XR_XIR_BAD_STRUCTURE;
}

static XrXirStatus effect_body_run_word(const EffectBodyRead *read,const EffectBodyRunRef *ref,
    uint32_t instruction,uint32_t word,uint32_t *output) {
    if (!ref || !output || word>=EFFECT_BODY_WORDS || instruction<ref->first ||
        instruction-ref->first>=ref->count) return XR_XIR_BAD_STRUCTURE;
    uint32_t at=0;
    for (uint32_t w=0;w<word;++w) {
        if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
        at+=!!(ref->mask&(UINT32_C(1)<<w));
    }
    bool delta=!!(ref->mask&(UINT32_C(1)<<word));
    if (!effect_body_read_work(read,1+(delta?3:0)+sizeof(*output))) return XR_XIR_BUDGET;
    *output=ref->base[word]+(delta?ref->delta[at]*(instruction-ref->first):0);
    return XR_XIR_OK;
}

typedef struct EffectBodyInstructionFields {
    XrXirOp op;
    XrXirType type;
    uint32_t source;
} EffectBodyInstructionFields;

/* Both projections read every physical word through the same canonical
 * run, neighbor and range checks. Only the requested output is published. */
static XrXirStatus effect_body_instruction_words_get(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,uint32_t *words) {
    if (!words) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRunRef ref;
    XrXirStatus status=effect_body_record_ref(read,function,instruction,&ref);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(read,sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t delta_index=0;
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        bool present=!!(ref.mask&(UINT32_C(1)<<w));
        if (!effect_body_read_work(read,1+(present?4:0)+sizeof(words[w]))) return XR_XIR_BUDGET;
        words[w]=ref.base[w]+(present?ref.delta[delta_index++]*(instruction-ref.first):0);
    }
    if (words[0]<=XR_XIR_INVALID || words[0]>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    return XR_XIR_OK;
}

static XrXirStatus effect_body_instruction_get(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,XrXirInstruction *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    uint32_t words[EFFECT_BODY_WORDS];
    XrXirStatus status=effect_body_instruction_words_get(read,function,instruction,words);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(read,sizeof(*output)+2)) return XR_XIR_BUDGET;
    uint64_t immediate=(uint64_t)words[6]|((uint64_t)words[7]<<32);int64_t signed_immediate;
    memcpy(&signed_immediate,&immediate,sizeof(immediate));
    *output=(XrXirInstruction){.op=(XrXirOp)words[0],.type=words[1],
        .args={words[2],words[3]},.targets={words[4],words[5]},
        .immediate=signed_immediate,.type_arguments={words[8],words[9]}};
    return XR_XIR_OK;
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

static XrXirStatus effect_body_operand_type(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t value,XrXirType *output) {
    if (!function || !output || value>=(uint64_t)function->parameter_count+function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    if (value<function->parameter_count) {
        if (!function->parameters) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,1+sizeof(*output))) return XR_XIR_BUDGET;
        *output=function->parameters[value];return XR_XIR_OK;
    }
    EffectBodyRunRef ref;uint32_t type=0;
    XrXirStatus status=effect_body_record_ref(read,function,value-function->parameter_count,&ref);
    if (status==XR_XIR_OK) status=effect_body_run_word(read,&ref,value-function->parameter_count,1,&type);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=(XrXirType)type;return XR_XIR_OK;
}

static XrXirStatus effect_body_instruction_op(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t instruction,XrXirOp *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    EffectBodyRunRef ref;
    if (!effect_body_read_work(read,sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t opcode=XR_XIR_INVALID;
    XrXirStatus status=effect_body_record_ref(read,function,instruction,&ref);
    if (status==XR_XIR_OK) status=effect_body_run_word(read,&ref,instruction,0,&opcode);
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
    EffectBodyRunRef ref;uint32_t operation=XR_XIR_INVALID,physical_type=XR_XIR_UNIT;
    if (!effect_body_read_work(read,2*sizeof(uint32_t))) return XR_XIR_BUDGET;
    XrXirStatus status=effect_body_record_ref(read,function,instruction,&ref);
    if (status==XR_XIR_OK) status=effect_body_run_word(read,&ref,instruction,0,&operation);
    if (status==XR_XIR_OK) status=effect_body_run_word(read,&ref,instruction,1,&physical_type);
    if (status!=XR_XIR_OK) return status;
    if (operation<=XR_XIR_INVALID || operation>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,2*sizeof(*opcode))) return XR_XIR_BUDGET;
    *opcode=operation;*type=physical_type;return XR_XIR_OK;
}

/* Independent correspondence is linear in physical rows and fields. It
 * validates every run boundary and all words, even unused instruction words. */
static XrXirStatus effect_body_records_match(const EffectBodyRead *read,
    const EffectBodyFunction *function,const XrXirFunction *source) {
    if (!function || !source || function->instruction_count!=source->instruction_count ||
        (source->instruction_count && !source->instructions) ||
        function->run_count>function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    uint32_t first=0,offset=0;
    for (uint32_t r=0;r<function->run_count;++r) {
        EffectBodyRunRef ref;
        XrXirStatus status=effect_body_run_ref(read,function,offset,first,&ref);
        if (status!=XR_XIR_OK) return status;
        for (uint32_t i=first;i-first<ref.count;++i) {
            uint32_t at=0;
            for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                bool delta=!!(ref.mask&(UINT32_C(1)<<w));
                if (!effect_body_read_work(read,3+(delta?4:0))) return XR_XIR_BUDGET;
                if (ref.base[w]+(delta?ref.delta[at++]*(i-first):0)!=
                    effect_body_source_word(&source->instructions[i],w)) return XR_XIR_BAD_STRUCTURE;
            }
        }
        first+=ref.count;offset=ref.next;
    }
    return first==function->instruction_count && offset==function->stream_words?
        XR_XIR_OK:XR_XIR_BAD_STRUCTURE;
}
