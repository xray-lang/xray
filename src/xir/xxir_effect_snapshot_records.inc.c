/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_effect_snapshot_records.inc.c - Lossless physical instruction records
 *
 * KEY CONCEPT:
 *   Complete literals and unsigned affine arithmetic preserve all ten physical
 *   words, including unused and immediate bits. Types never select a codec.
 */
#include "xxir_instruction_words_internal.h"

/* A literal tag selects complete physical rows, never constant columns. All
 * actual opcodes are bounded here; full correspondence reads every word. */
static XrXirStatus effect_body_run_payload_check(const EffectBodyRead *read,
    const EffectBodyRun *run) {
    if (!read || !run || !run->count ||
        (run->mask&~(EFFECT_BODY_MASK|EFFECT_BODY_LITERAL_ROWS)) ||
        (run->count==1 && run->mask)) return XR_XIR_BAD_STRUCTURE;
    uint32_t selected=run->mask&EFFECT_BODY_MASK;
    if (run->mask&EFFECT_BODY_LITERAL_ROWS) {
        if (run->count<2 || run->count>EFFECT_BODY_LITERAL_CAPACITY || selected) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,2)) return XR_XIR_BUDGET;
        if (run->base.op<=XR_XIR_INVALID || run->base.op>=XR_XIR_OP_COUNT)
            return XR_XIR_BAD_STRUCTURE;
        for (uint32_t i=1;i<run->count;++i) {
            if (!effect_body_read_work(read,2)) return XR_XIR_BUDGET;
            if (run->literal_tail[i-1].op<=XR_XIR_INVALID || run->literal_tail[i-1].op>=XR_XIR_OP_COUNT)
                return XR_XIR_BAD_STRUCTURE;
        }
        return XR_XIR_OK;
    }
    if (run->count==2 && selected) return XR_XIR_BAD_STRUCTURE;
    for (;selected;selected&=selected-1) {
        if (!effect_body_read_work(read,3)) return XR_XIR_BUDGET;
        if (!run->delta[(uint32_t)__builtin_ctz(selected)]) return XR_XIR_BAD_STRUCTURE;
    }
    return XR_XIR_OK;
}

/* Both growth and immutable receiving copy initialized fields only. Inactive
 * delta words and unused slots are not data and never enter either copy. */
static XrXirStatus effect_body_run_copy_initialized(const XrXirCompileContext *work,
    const EffectBodyRun *source,EffectBodyRun *target) {
    if (!source || !target || !source->count ||
        (source->mask&~(EFFECT_BODY_MASK|EFFECT_BODY_LITERAL_ROWS)) ||
        (source->count==1 && source->mask) ||
        ((source->mask&EFFECT_BODY_LITERAL_ROWS) &&
            (source->count<2 || source->count>EFFECT_BODY_LITERAL_CAPACITY || (source->mask&EFFECT_BODY_MASK))))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,3*sizeof(uint32_t)+sizeof(source->base))) return XR_XIR_BUDGET;
    target->first=source->first;target->count=source->count;target->mask=source->mask;
    target->base=source->base;
    if (source->mask&EFFECT_BODY_LITERAL_ROWS) {
        for (uint32_t i=1;i<source->count;++i) {
            if (!xir_compile_work(work,1+sizeof(source->literal_tail[i-1]))) return XR_XIR_BUDGET;
            target->literal_tail[i-1]=source->literal_tail[i-1];
        }
        return XR_XIR_OK;
    }
    for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (!(source->mask&(UINT32_C(1)<<w))) continue;
        if (!xir_compile_work(work,sizeof(uint32_t))) return XR_XIR_BUDGET;
        target->delta[w]=source->delta[w];
    }
    return XR_XIR_OK;
}

static XrXirStatus effect_body_runs_copy(const XrXirCompileContext *work,
    const EffectBodyFunction *function,EffectBodyRun *storage) {
    for (uint32_t r=0;r<function->run_count;++r) {
        XrXirStatus status=effect_body_run_copy_initialized(work,&function->runs[r],&storage[r]);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* Unused capacity is never read or traversed. A failed replacement retains
 * the previous allocation so the unique material destructor can release it. */
static XrXirStatus effect_body_runs_replace(const XrXirCompileContext *work,
    EffectBodyFunction *function,uint32_t capacity) {
    if (capacity<function->run_count || (uint64_t)capacity>SIZE_MAX/sizeof(*function->runs))
        return XR_XIR_BUDGET;
    XrXirStatus status=XR_XIR_OK;
    EffectBodyRun *storage=xir_compile_alloc(work,(size_t)capacity*sizeof(*storage),&status);
    if (!storage) return status;
    if (!xir_compile_work(work,3)) { xr_compile_resources_free(storage);return XR_XIR_BUDGET; }
    status=effect_body_runs_copy(work,function,storage);
    if (status!=XR_XIR_OK) { xr_compile_resources_free(storage);return status; }
    xr_compile_resources_free(function->runs);function->runs=storage;
    function->run_capacity=capacity;return XR_XIR_OK;
}

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
    return effect_body_runs_replace(work,function,capacity);
}

static XrXirStatus effect_body_runs_finish(const XrXirCompileContext *work,
    EffectBodyFunction *function) {
    if (!function || function->run_count>function->run_capacity ||
        function->run_capacity>function->instruction_count ||
        !!function->runs!=!!function->run_capacity ||
        (!function->run_count && function->run_capacity)) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,sizeof(function->run_capacity))) return XR_XIR_BUDGET;
    if (function->run_count==function->run_capacity) return XR_XIR_OK;
    return effect_body_runs_replace(work,function,function->run_count);
}

/* Only completed scalar-only records join the current literal tail. A
 * uniform-span record remains a stable boundary, so a semantic cursor never
 * changes its preceding directory positions. The input is a local value and
 * no array element reference survives reserve or immutable replacement. */
static XrXirStatus effect_body_short_literals_append(const XrXirCompileContext *work,
    EffectBodyFunction *function,const EffectBodyRun *run) {
    for (uint32_t i=0;i<run->count;++i) {
        if (!xir_compile_work(work,3+sizeof(uint32_t)+sizeof(const XrXirInstruction *))) return XR_XIR_BUDGET;
        const XrXirInstruction *row=i?&run->literal_tail[i-1]:&run->base;
        uint32_t first=run->first+i;
        if (function->run_count) {
            EffectBodyRun *last=&function->runs[function->run_count-1];
            if (!xir_compile_work(work,7)) return XR_XIR_BUDGET;
            if (!last->count || last->first>=function->instruction_count ||
                last->count>function->instruction_count-last->first || last->first+last->count!=first)
                return XR_XIR_BAD_STRUCTURE;
            if (last->count<EFFECT_BODY_LITERAL_CAPACITY &&
                (last->mask==EFFECT_BODY_LITERAL_ROWS || (last->count==1 && !last->mask))) {
                if (!xir_compile_work(work,sizeof(*row)+2*sizeof(uint32_t)+1)) return XR_XIR_BUDGET;
                last->literal_tail[last->count-1]=*row;++last->count;last->mask=EFFECT_BODY_LITERAL_ROWS;continue;
            }
        } else if (first) return XR_XIR_BAD_STRUCTURE;
        if (function->run_count>=function->run_capacity || !function->runs) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(work,3*sizeof(uint32_t)+sizeof(*row)+1)) return XR_XIR_BUDGET;
        EffectBodyRun *target=&function->runs[function->run_count];
        target->first=first;target->count=1;target->mask=0;target->base=*row;++function->run_count;
    }
    return XR_XIR_OK;
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
        run->count>function->instruction_count-run->first)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyRead read=effect_body_compile_read(work);
    XrXirStatus status=effect_body_run_payload_check(&read,run);
    if (status==XR_XIR_OK) status=effect_body_runs_reserve(work,function);
    if (status!=XR_XIR_OK) return status;
    EffectBodyRun *target=&function->runs[function->run_count];
    if (!xir_compile_work(work,3*sizeof(uint32_t)+sizeof(run->base)+1)) return XR_XIR_BUDGET;
    target->first=run->first;target->count=run->count;target->mask=run->mask;
    target->base=run->base;
    if (run->mask&EFFECT_BODY_LITERAL_ROWS) {
        for (uint32_t i=1;i<run->count;++i) {
            if (!xir_compile_work(work,1+sizeof(run->literal_tail[i-1]))) return XR_XIR_BUDGET;
            target->literal_tail[i-1]=run->literal_tail[i-1];
        }
    } else for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (!(run->mask&(UINT32_C(1)<<w))) continue;
        if (!xir_compile_work(work,sizeof(uint32_t))) return XR_XIR_BUDGET;
        target->delta[w]=run->delta[w];
    }
    ++function->run_count;
    if (run->first+run->count==function->instruction_count) {
        return effect_body_runs_finish(work,function);
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

typedef struct EffectBodyRecordSink {
    void *owner;
    XrXirStatus (*run)(const XrXirCompileContext *,const XrXirFunction *,EffectBodyRun *,void *);
} EffectBodyRecordSink;
typedef struct EffectBodyRecordCensus { uint32_t next,count,tail; } EffectBodyRecordCensus;
typedef struct EffectBodyRecordReceiver {
    EffectBodyFunction *target;
    const EffectBodyWriteSink *semantic;
} EffectBodyRecordReceiver;

static XrXirStatus effect_body_record_prepare(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyRun *run) {
    if (!source || !run || !source->instructions || !run->count || run->first>=source->instruction_count ||
        run->count>source->instruction_count-run->first ||
        (run->mask&~(EFFECT_BODY_MASK|EFFECT_BODY_LITERAL_ROWS)) || (run->count==1 && run->mask) ||
        ((run->mask&EFFECT_BODY_LITERAL_ROWS) && (run->mask&EFFECT_BODY_MASK))) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,4)) return XR_XIR_BUDGET;
    if (run->count==2 && run->mask) {
        if (!xir_compile_work(work,2+sizeof(run->literal_tail[0])+sizeof(run->mask))) return XR_XIR_BUDGET;
        run->literal_tail[0]=source->instructions[run->first+1];run->mask=EFFECT_BODY_LITERAL_ROWS;
    }
    EffectBodyRead read=effect_body_compile_read(work);
    return effect_body_run_payload_check(&read,run);
}

/* This pass measures only physical capacity. The same scanner runs afresh
 * for receiving, and neither its count nor its discarded payload proves IR,
 * permission, semantic shape or correspondence to the current source. */
static XrXirStatus effect_body_record_count(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyRun *run,void *owner) {
    EffectBodyRecordCensus *census=owner;
    XrXirStatus status=effect_body_record_prepare(work,source,run);
    if (status!=XR_XIR_OK) return status;
    if (!census || census->next!=run->first || census->count>census->next ||
        census->tail>EFFECT_BODY_LITERAL_CAPACITY) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,6+sizeof(uint32_t))) return XR_XIR_BUDGET;
    if (run->count==1 || (run->mask&EFFECT_BODY_LITERAL_ROWS)) {
        for (uint32_t i=0;i<run->count;++i) {
            if (!xir_compile_work(work,6+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
            if (census->tail && census->tail<EFFECT_BODY_LITERAL_CAPACITY) ++census->tail;
            else { if (census->count>=source->instruction_count) return XR_XIR_BAD_STRUCTURE;
                ++census->count;census->tail=1; }
        }
    } else {
        if (census->count>=source->instruction_count) return XR_XIR_BAD_STRUCTURE;
        if (!xir_compile_work(work,2*sizeof(uint32_t))) return XR_XIR_BUDGET;
        ++census->count;census->tail=0;
    }
    census->next+=run->count;return XR_XIR_OK;
}

static XrXirStatus effect_body_run_deliver(const XrXirCompileContext *work,
    const XrXirFunction *source,const EffectBodyRun *run,const EffectBodyWriteSink *sink) {
    if (!sink) return XR_XIR_OK;
    if (!sink->owner || !sink->row || !sink->span) return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,5+3*sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t first=run->first,count=run->count,mask=run->mask;
    if (count>1 && !(mask&EFFECT_BODY_LITERAL_ROWS) && !(mask&UINT32_C(3)))
        return sink->span(sink->owner,first,count);
    for (uint32_t i=first;i<first+count;++i) {
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        XrXirStatus status=sink->row(sink->owner,i,&source->instructions[i]);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
}

/* Semantic intake follows the complete physical comparison and append.
 * A constant-column mask chooses only the private range reader. That reader
 * independently checks the real fields; all other rows retain scalar intake. */
static XrXirStatus effect_body_run_submit(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyFunction *target,EffectBodyRun *run,
    const EffectBodyWriteSink *sink) {
    if (!source || !target || !run || target->run_count>target->run_capacity ||
        target->run_capacity>target->instruction_count || (target->run_capacity && !target->runs) ||
        !run->count || run->first>=target->instruction_count || run->count>target->instruction_count-run->first ||
        (run->mask&~(EFFECT_BODY_MASK|EFFECT_BODY_LITERAL_ROWS)) || (run->count==1 && run->mask) ||
        ((run->mask&EFFECT_BODY_LITERAL_ROWS) && (run->mask&EFFECT_BODY_MASK)))
        return XR_XIR_BAD_STRUCTURE;
    if (!xir_compile_work(work,3+sizeof(uint32_t)+2*sizeof(uintptr_t))) return XR_XIR_BUDGET;
    uintptr_t address=(uintptr_t)run,begin=(uintptr_t)target->runs;
    if (target->runs && address>=begin && (address-begin)/sizeof(*run)<target->run_capacity)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t count=run->count;
    XrXirStatus status=effect_body_record_prepare(work,source,run);
    if (status!=XR_XIR_OK) return status;
    if (count==1 || (run->mask&EFFECT_BODY_LITERAL_ROWS)) status=effect_body_short_literals_append(work,target,run);
    else {
        if (target->run_count>=target->run_capacity || !target->runs) return XR_XIR_BAD_STRUCTURE;
        status=effect_body_run_copy_initialized(work,run,&target->runs[target->run_count]);
        if (status==XR_XIR_OK && !xir_compile_work(work,1)) status=XR_XIR_BUDGET;
        if (status==XR_XIR_OK) ++target->run_count;
    }
    return status==XR_XIR_OK?effect_body_run_deliver(work,source,run,sink):status;
}

static XrXirStatus effect_body_record_receive(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyRun *run,void *owner) {
    const EffectBodyRecordReceiver *receiver=owner;
    if (!receiver) return XR_XIR_BAD_STRUCTURE;
    return effect_body_run_submit(work,source,receiver->target,run,receiver->semantic);
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
static XrXirStatus effect_body_records_scan(const XrXirCompileContext *work,
    const XrXirFunction *source,const EffectBodyRecordSink *sink) {
    if (!source || !sink || !sink->owner || !sink->run || (source->instruction_count && !source->instructions))
        return XR_XIR_BAD_STRUCTURE;
    if (!source->instruction_count) return XR_XIR_OK;
    if (!xir_compile_work(work,sizeof(uint32_t)+sizeof(EffectBodyRun *))) return XR_XIR_BUDGET;
    uint32_t column=UINT32_MAX;EffectBodyRun pending,*run=NULL;
    for (uint32_t i=0;i<source->instruction_count;++i) {
        const XrXirInstruction *op=&source->instructions[i];
        if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
        if (!run || run->count<2 || (run->mask&UINT32_C(1))) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
        }
        if (!run) {
            if (!xir_compile_work(work,sizeof(run))) return XR_XIR_BUDGET;
            run=&pending;
            XrXirStatus status=effect_body_run_seed(work,run,op,i);
            if (status!=XR_XIR_OK) return status;
        } else if (run->count==1) {
            if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
            for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
                uint32_t delta=xir_instruction_word(op,w)-xir_instruction_word(&run->base,w);
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
                    if (xir_instruction_word(op,w)!=xir_instruction_word(&run->base,w)) goto new_run;
                }
                if (column<EFFECT_BODY_WORDS) {
                    if (!xir_compile_work(work,3)) return XR_XIR_BUDGET;
                    uint32_t expected=xir_instruction_word(&run->base,column)+run->delta[column]*run->count;
                    if (xir_instruction_word(op,column)!=expected) goto new_run;
                }
                for (uint32_t w=column+1;w<EFFECT_BODY_WORDS;++w) {
                    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
                    if (xir_instruction_word(op,w)!=xir_instruction_word(&run->base,w)) goto new_run;
                }
            } else {
                for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                    bool varying=!!(run->mask&(UINT32_C(1)<<w));
                    if (!xir_compile_work(work,2+(varying?2:0))) return XR_XIR_BUDGET;
                    uint32_t expected=xir_instruction_word(&run->base,w);
                    if (varying) expected+=run->delta[w]*run->count;
                    if (xir_instruction_word(op,w)!=expected) goto new_run;
                }
            }
            if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
            ++run->count;continue;
        new_run:
            if (!(run->mask&UINT32_C(1))) {
                if (!xir_compile_work(work,2)) return XR_XIR_BUDGET;
                if (op->op<=XR_XIR_INVALID || op->op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
            }
            XrXirStatus status=sink->run(work,source,run,sink->owner);
            if (status==XR_XIR_OK && !xir_compile_work(work,sizeof(run))) status=XR_XIR_BUDGET;
            if (status==XR_XIR_OK) { run=&pending;
                status=effect_body_run_seed(work,run,op,i); }
            if (status!=XR_XIR_OK) return status;
        }
    }
    return sink->run(work,source,run,sink->owner);
}

/* Capacity is measured before allocation, then every current source word
 * is read again by the same scanner. An altered count cannot grow receiving
 * storage or publish a body. Independent semantic EOF remains with its owner. */
static XrXirStatus effect_body_records_write(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyFunction *target,const EffectBodyWriteSink *sink) {
    if (!source || !target || source->instruction_count!=target->instruction_count || target->run_count ||
        target->run_capacity || target->runs || (source->instruction_count && !source->instructions))
        return XR_XIR_BAD_STRUCTURE;
    if (!source->instruction_count) return XR_XIR_OK;
    if (!xir_compile_work(work,sizeof(EffectBodyRecordCensus)+2*sizeof(EffectBodyRecordSink)+sizeof(EffectBodyRecordReceiver)))
        return XR_XIR_BUDGET;
    EffectBodyRecordCensus census={0};EffectBodyRecordSink counter={&census,effect_body_record_count};
    XrXirStatus status=effect_body_records_scan(work,source,&counter);
    if (status!=XR_XIR_OK) return status;
    if (!census.count || census.next!=source->instruction_count) return XR_XIR_BAD_STRUCTURE;
    status=effect_body_runs_replace(work,target,census.count);
    if (status!=XR_XIR_OK) return status;
    EffectBodyRecordReceiver receiver={target,sink};EffectBodyRecordSink receiving={&receiver,effect_body_record_receive};
    status=effect_body_records_scan(work,source,&receiving);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,1)) return XR_XIR_BUDGET;
    if (target->run_count!=census.count) return XR_XIR_BAD_STRUCTURE;
    /* The independent semantic function EOF still precedes publication of
     * its complete owner. A physical census cannot replace that operation. */
    if (sink) return XR_XIR_OK;
    /* The same immutable extent gate now verifies an already exact allocation. */
    return effect_body_runs_finish(work,target);
}

/* Literal codec construction does not publish a semantic generation. */
static inline XrXirStatus effect_body_records_copy(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyFunction *target) {
    return effect_body_records_write(work,source,target,NULL);
}

/* Canonical physical fields share one check for random and complete walks. */
static XrXirStatus effect_body_run_check(const EffectBodyRead *read,
    const EffectBodyRun *run,uint32_t first,uint32_t instructions) {
    if (!read || !run || first>=instructions) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,7)) return XR_XIR_BUDGET;
    if (run->first!=first || !run->count || run->count>instructions-first ||
        (run->mask&~(EFFECT_BODY_MASK|EFFECT_BODY_LITERAL_ROWS))) return XR_XIR_BAD_STRUCTURE;
    return effect_body_run_payload_check(read,run);
}

/* The same bounded reader serves compile and runtime ledger callbacks. A
 * local run reference cannot outlive this request or acquire body permission. */
static XrXirStatus effect_body_run_ref(const EffectBodyRead *read,
    const EffectBodyFunction *function,uint32_t index,uint32_t first,const EffectBodyRun **output) {
    if (!read || !output || !function || !function->runs ||
        function->run_count>function->run_capacity || function->run_count>function->instruction_count ||
        index>=function->run_count || first>=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    const EffectBodyRun *run=&function->runs[index];
    XrXirStatus status=effect_body_run_check(read,run,first,function->instruction_count);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
    *output=run;return XR_XIR_OK;
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
    if (!effect_body_read_work(read,3*sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t low=0,high=function->run_count,first=0;
    const EffectBodyRun *first_boundary,*last_boundary,*boundary;
    XrXirStatus status=effect_body_run_ref(read,function,0,0,&first_boundary);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(read,2)) return XR_XIR_BUDGET;
    if (function->run_count==1) {
        if (first_boundary->count!=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
        *output=first_boundary;return XR_XIR_OK;
    } else {
        uint32_t last=function->run_count-1;
        if (!effect_body_read_work(read,sizeof(last)+1)) return XR_XIR_BUDGET;
        status=effect_body_run_ref(read,function,last,function->runs[last].first,&last_boundary);
        if (status!=XR_XIR_OK) return status;
    }
    if (last_boundary->first+last_boundary->count!=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    while (low<high) {
        if (!effect_body_read_work(read,3+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
        uint32_t middle=low+(high-low)/2;
        if (function->runs[middle].first<=instruction) low=middle+1;
        else high=middle;
    }
    if (!low) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,1+sizeof(uint32_t))) return XR_XIR_BUDGET;
    uint32_t index=low-1;
    if (index) {
        if (index==1) {
            if (!effect_body_read_work(read,sizeof(boundary))) return XR_XIR_BUDGET;
            boundary=first_boundary;
        } else status=effect_body_run_ref(read,function,index-1,function->runs[index-1].first,&boundary);
        if (status!=XR_XIR_OK) return status;
        if (!effect_body_read_work(read,1+sizeof(first))) return XR_XIR_BUDGET;
        first=boundary->first+boundary->count;
    }
    const EffectBodyRun *ref;
    /* These boundary records were fully read in this same immutable request.
     * Reuse their physical data while still checking the selected adjacency. */
    if (!index || index+1==function->run_count) {
        if (!effect_body_read_work(read,sizeof(ref)+1)) return XR_XIR_BUDGET;
        ref=index?last_boundary:first_boundary;
        if (ref->first!=first) return XR_XIR_BAD_STRUCTURE;
    } else status=effect_body_run_ref(read,function,index,first,&ref);
    if (status!=XR_XIR_OK) return status;
    if (!effect_body_read_work(read,3)) return XR_XIR_BUDGET;
    if (instruction<ref->first || instruction-ref->first>=ref->count) return XR_XIR_BAD_STRUCTURE;
    if (index+1<function->run_count) {
        if (index+2==function->run_count) {
            if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
            if (last_boundary->first!=ref->first+ref->count) return XR_XIR_BAD_STRUCTURE;
        } else status=effect_body_run_ref(read,function,index+1,ref->first+ref->count,&boundary);
        if (status!=XR_XIR_OK) return status;
    } else if (ref->first+ref->count!=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
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

typedef struct EffectBodyRunVisitor {
    void *owner;
    XrXirStatus (*run)(void *,const EffectBodyRun *);
} EffectBodyRunVisitor;

/* The immutable material is retained throughout this synchronous call.
 * Validate the complete current directory before any consumer runs, including
 * every selected delta, adjacent boundary and exact EOF. Only actual current
 * run references enter the visitor; no completed flag survives this call. */
static XrXirStatus effect_body_runs_visit(const EffectBodyRead *read,
    const EffectBodyFunction *function,const EffectBodyRunVisitor *visitor) {
    if (!read || !function || !visitor || !visitor->owner || !visitor->run ||
        (!function->instruction_count && (function->runs || function->run_count || function->run_capacity)) ||
        (function->instruction_count && (!function->runs || !function->run_count)) ||
        function->run_count>function->run_capacity || function->run_count>function->instruction_count)
        return XR_XIR_BAD_STRUCTURE;
    uint32_t first=0;
    if (!effect_body_read_work(read,7+sizeof(first))) return XR_XIR_BUDGET;
    for (uint32_t r=0;r<function->run_count;++r) {
        const EffectBodyRun *run=&function->runs[r];
        XrXirStatus status=effect_body_run_check(read,run,first,function->instruction_count);
        if (status!=XR_XIR_OK) return status;
        if (!effect_body_read_work(read,2+2*sizeof(uint32_t))) return XR_XIR_BUDGET;
        first+=run->count;
    }
    if (!effect_body_read_work(read,1)) return XR_XIR_BUDGET;
    if (first!=function->instruction_count) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t r=0;r<function->run_count;++r) {
        if (!effect_body_read_work(read,2+sizeof(void *))) return XR_XIR_BUDGET;
        XrXirStatus status=visitor->run(visitor->owner,&function->runs[r]);
        if (status!=XR_XIR_OK) return status;
    }
    return XR_XIR_OK;
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

static const XrXirInstruction *effect_body_literal_row(const EffectBodyRun *ref,uint32_t instruction) {
    return instruction==ref->first?&ref->base:&ref->literal_tail[instruction-ref->first-1];
}

static XrXirStatus effect_body_run_word(const EffectBodyRead *read,const EffectBodyRun *ref,
    uint32_t instruction,uint32_t word,uint32_t *output) {
    if (!ref || !output || word>=EFFECT_BODY_WORDS || instruction<ref->first ||
        instruction-ref->first>=ref->count) return XR_XIR_BAD_STRUCTURE;
    if (ref->mask&EFFECT_BODY_LITERAL_ROWS) {
        if (ref->count<2 || ref->count>EFFECT_BODY_LITERAL_CAPACITY || ref->mask!=EFFECT_BODY_LITERAL_ROWS)
            return XR_XIR_BAD_STRUCTURE;
        if (!effect_body_read_work(read,3+sizeof(*output))) return XR_XIR_BUDGET;
        *output=xir_instruction_word(effect_body_literal_row(ref,instruction),word);
        return XR_XIR_OK;
    }
    bool delta=!!(ref->mask&(UINT32_C(1)<<word));
    if (!effect_body_read_work(read,1+(delta?3:0)+sizeof(*output))) return XR_XIR_BUDGET;
    *output=xir_instruction_word(&ref->base,word)+(delta?ref->delta[word]*(instruction-ref->first):0);
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
        if (ref->mask&EFFECT_BODY_LITERAL_ROWS) {
            if (ref->count<2 || ref->count>EFFECT_BODY_LITERAL_CAPACITY || ref->mask!=EFFECT_BODY_LITERAL_ROWS)
                return XR_XIR_BAD_STRUCTURE;
            if (!effect_body_read_work(read,3+sizeof(words[w]))) return XR_XIR_BUDGET;
            words[w]=xir_instruction_word(effect_body_literal_row(ref,instruction),w);continue;
        }
        bool present=!!(ref->mask&(UINT32_C(1)<<w));
        if (!effect_body_read_work(read,1+(present?4:0)+sizeof(words[w]))) return XR_XIR_BUDGET;
        words[w]=xir_instruction_word(&ref->base,w)+(present?ref->delta[w]*(instruction-ref->first):0);
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


/* Counting and opcode validation finish before the full row is published.
 * The value core writes no temporary ten-word array; its actual cursor,
 * selected reads and complete Instruction output remain charged. */
static XrXirStatus effect_body_instruction_ref_prepare(const EffectBodyRead *read,
    const EffectBodyRun *ref,uint32_t instruction) {
    if (!read || !ref || (ref->mask&~(EFFECT_BODY_MASK|EFFECT_BODY_LITERAL_ROWS)) ||
        (ref->count==1 && ref->mask) ||
        instruction<ref->first || instruction-ref->first>=ref->count) return XR_XIR_BAD_STRUCTURE;
    if (ref->mask&EFFECT_BODY_LITERAL_ROWS) {
        XrXirStatus status=effect_body_run_payload_check(read,ref);
        if (status!=XR_XIR_OK) return status;
        return effect_body_read_work(read,1+sizeof(XrXirInstruction))?XR_XIR_OK:XR_XIR_BUDGET;
    }
    if (ref->count==2 && ref->mask) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,1+sizeof(uint32_t)+((ref->mask&UINT32_C(1))?3:0)))
        return XR_XIR_BUDGET;
    uint32_t operation=xir_instruction_word(&ref->base,0)+((ref->mask&UINT32_C(1))?
        ref->delta[0]*(instruction-ref->first):0);
    if (operation<=XR_XIR_INVALID || operation>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    uint64_t units=EFFECT_BODY_WORDS+sizeof(XrXirInstruction)+2;
    for (uint32_t selected=ref->mask;selected;selected&=selected-1) {
        if (!effect_body_read_work(read,3)) return XR_XIR_BUDGET;
        uint32_t w=(uint32_t)__builtin_ctz(selected);
        if (!ref->delta[w]) return XR_XIR_BAD_STRUCTURE;
        units+=4;
    }
    if (!effect_body_read_work(read,units+EFFECT_BODY_WORDS)) return XR_XIR_BUDGET;
    return XR_XIR_OK;
}
static XrXirStatus effect_body_instruction_ref_value(const EffectBodyRead *read,
    const EffectBodyRun *ref,uint32_t instruction,XrXirInstruction *output) {
    if (!output) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_instruction_ref_prepare(read,ref,instruction);
    if (status!=XR_XIR_OK) return status;
    if (ref->mask&EFFECT_BODY_LITERAL_ROWS) *output=*effect_body_literal_row(ref,instruction);
    else *output=xir_instruction_words_value(&ref->base,NULL,ref->mask,ref->delta,instruction-ref->first);
    return XR_XIR_OK;
}

/* A current canonical first or constant row already exists in its retained
 * material. Borrow it only for the synchronous consumer; varying later rows
 * still use the sole complete value decoder and caller-owned scratch. */
static XrXirStatus effect_body_instruction_ref_read(const EffectBodyRead *read,
    const EffectBodyRun *ref,uint32_t instruction,XrXirInstruction *storage,
    const XrXirInstruction **output) {
    if (!read || !ref || !storage || !output || (ref->mask&~(EFFECT_BODY_MASK|EFFECT_BODY_LITERAL_ROWS)) ||
        (ref->count==1 && ref->mask) || instruction<ref->first ||
        instruction-ref->first>=ref->count) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,sizeof(*output))) return XR_XIR_BUDGET;
    if (ref->mask&EFFECT_BODY_LITERAL_ROWS) {
        XrXirStatus status=effect_body_run_payload_check(read,ref);
        if (status!=XR_XIR_OK) return status;
        if (!effect_body_read_work(read,2)) return XR_XIR_BUDGET;
        *output=effect_body_literal_row(ref,instruction);return XR_XIR_OK;
    }
    if (ref->count==2 && ref->mask) return XR_XIR_BAD_STRUCTURE;
    if (instruction!=ref->first && ref->mask) {
        XrXirStatus status=effect_body_instruction_ref_value(read,ref,instruction,storage);
        if (status!=XR_XIR_OK) return status;
        *output=storage;return XR_XIR_OK;
    }
    if (!effect_body_read_work(read,1+sizeof(uint32_t))) return XR_XIR_BUDGET;
    if (ref->base.op<=XR_XIR_INVALID || ref->base.op>=XR_XIR_OP_COUNT) return XR_XIR_BAD_STRUCTURE;
    for (uint32_t selected=ref->mask;selected;selected&=selected-1) {
        if (!effect_body_read_work(read,3)) return XR_XIR_BUDGET;
        if (!ref->delta[(uint32_t)__builtin_ctz(selected)]) return XR_XIR_BAD_STRUCTURE;
    }
    *output=&ref->base;return XR_XIR_OK;
}

static inline XrXirStatus effect_body_instruction_words_publish(const EffectBodyRead *read,
    const uint32_t *words,XrXirInstruction *output) {
    if (!words || !output) return XR_XIR_BAD_STRUCTURE;
    if (!effect_body_read_work(read,sizeof(uint32_t)+2*EFFECT_BODY_WORDS+sizeof(*output)+2))
        return XR_XIR_BUDGET;
    *output=xir_instruction_words_value(NULL,words,0,NULL,0);return XR_XIR_OK;
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
            if (xir_instruction_word(&ref->base,w)!=xir_instruction_word(&source->instructions[i],w)) {
                if (!same) return XR_XIR_BAD_STRUCTURE;
                if (!effect_body_read_work(read,sizeof(*same))) return XR_XIR_BUDGET;
                *same=false;
            }
        }
        if (column<EFFECT_BODY_WORDS) {
            if (!effect_body_read_work(read,6)) return XR_XIR_BUDGET;
            if (xir_instruction_word(&ref->base,column)+ref->delta[column]*(i-ref->first)!=
                xir_instruction_word(&source->instructions[i],column)) {
                if (!same) return XR_XIR_BAD_STRUCTURE;
                if (!effect_body_read_work(read,sizeof(*same))) return XR_XIR_BUDGET;
                *same=false;
            }
        }
        for (uint32_t w=column+1;w<EFFECT_BODY_WORDS;++w) {
            if (!effect_body_read_work(read,2)) return XR_XIR_BUDGET;
            if (xir_instruction_word(&ref->base,w)!=xir_instruction_word(&source->instructions[i],w)) {
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
        if (ref->count>2 && !(ref->mask&EFFECT_BODY_LITERAL_ROWS)) {
            status=effect_body_single_column(read,ref->mask,&column);
            if (status!=XR_XIR_OK) return status;
        }
        if (column<=EFFECT_BODY_WORDS) {
            status=effect_body_column_match(read,ref,source,column,same);
            if (status!=XR_XIR_OK) return status;
        } else for (uint32_t i=first;i-first<ref->count;++i) {
            for (uint32_t w=0;w<EFFECT_BODY_WORDS;++w) {
                bool delta=!!(ref->mask&(UINT32_C(1)<<w));
                bool pair=!!(ref->mask&EFFECT_BODY_LITERAL_ROWS);
                if (!effect_body_read_work(read,3+(pair?2:delta?4:0))) return XR_XIR_BUDGET;
                uint32_t actual=pair?xir_instruction_word(effect_body_literal_row(ref,i),w):
                    xir_instruction_word(&ref->base,w)+(delta?ref->delta[w]*(i-first):0);
                if (actual!=
                    xir_instruction_word(&source->instructions[i],w)) {
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

static XrXirStatus effect_body_record_replay(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyRun *run,void *owner) {
    const EffectBodyRecordReceiver *receiver=owner;
    if (!receiver) return XR_XIR_BAD_STRUCTURE;
    XrXirStatus status=effect_body_record_prepare(work,source,run);
    return status==XR_XIR_OK?effect_body_run_deliver(work,source,run,receiver->semantic):status;
}

/* The retained array is immutable data only. Fresh complete correspondence
 * precedes its attachment, then the original current-source scanner performs
 * every semantic intake again. Its independent function EOF remains separate. */
static XrXirStatus effect_body_records_receive(const XrXirCompileContext *work,
    const XrXirFunction *source,EffectBodyFunction *target,const EffectBodyFunction *retained,
    const EffectBodyWriteSink *sink) {
    if (!source || !target || !retained || target->run_count || target->run_capacity || target->runs ||
        target->instruction_count!=source->instruction_count || retained->run_count!=retained->run_capacity)
        return XR_XIR_BAD_STRUCTURE;
    EffectBodyRead read=effect_body_compile_read(work);
    XrXirStatus status=effect_body_records_match(&read,retained,source);
    if (status!=XR_XIR_OK) return status;
    if (!xir_compile_work(work,sizeof(EffectBodyRun *)+2*sizeof(uint32_t)+sizeof(EffectBodyRecordReceiver)+sizeof(EffectBodyRecordSink)))
        return XR_XIR_BUDGET;
    target->runs=retained->runs;target->run_count=retained->run_count;target->run_capacity=retained->run_capacity;
    EffectBodyRecordReceiver receiver={target,sink};EffectBodyRecordSink receiving={&receiver,effect_body_record_replay};
    return effect_body_records_scan(work,source,&receiving);
}
