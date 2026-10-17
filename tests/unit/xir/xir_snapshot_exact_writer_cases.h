/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_snapshot_exact_writer_cases.h - Physical census never replaces current intake
 */
#ifndef XIR_SNAPSHOT_EXACT_WRITER_CASES_H
#define XIR_SNAPSHOT_EXACT_WRITER_CASES_H
static XrXirInstruction *exact_writer_producer;
static unsigned exact_writer_mutation,exact_writer_allocations;
static void exact_writer_mutate_at_allocation(void *pointer) {
    (void)pointer;CHECK(exact_writer_producer && !exact_writer_allocations++);
    /* Deliberately violate source exclusion between the two physical passes.
     * The count must confer no authority and no stale payload may be replayed. */
    if(exact_writer_mutation==0)exact_writer_producer[16].op=XR_XIR_INVALID;
    else if(exact_writer_mutation==1)for(uint32_t i=0;i<17;++i)
        exact_writer_producer[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
    else if(exact_writer_mutation==2)literal_block_values(exact_writer_producer);
    else exact_writer_producer[9].args[1]^=UINT32_C(0x81000001);
}
static void snapshot_exact_writer_controls(void) {
    for(unsigned mode=0;mode<4;++mode){RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());
        uint64_t baseline=rp_stats(&context).live_bytes;XrXirInstruction source[17],expected[17];literal_block_values(source);
        if(mode==2)for(uint32_t i=0;i<17;++i)source[i]=(XrXirInstruction){.op=XR_XIR_CONST_INT,.type=XR_XIR_I64,.immediate=42};
        XrXirFunction input={.instructions=source,.instruction_count=17};EffectBodyFunction body={.instruction_count=17};
        CHECK(!rp_allocation_observer);exact_writer_mutation=mode;exact_writer_producer=source;exact_writer_allocations=0;
        rp_allocation_observer=exact_writer_mutate_at_allocation;XrXirStatus status=effect_body_records_copy(&context,&input,&body);
        rp_allocation_observer=NULL;exact_writer_producer=NULL;
        CHECK(exact_writer_allocations==1 && status==(mode<3?XR_XIR_BAD_STRUCTURE:XR_XIR_OK));
        CHECK(body.run_capacity==(mode==2?UINT32_C(1):UINT32_C(3)));
        if(mode==3){CHECK(body.run_count==3);literal_block_values(expected);expected[9].args[1]^=UINT32_C(0x81000001);
            for(uint32_t i=0;i<17;++i)CHECK(conditional_compact_same(&source[i],&expected[i]));memset(source,0xce,sizeof(source));
            EffectBodyRead read=effect_body_compile_read(&context);XrXirFunction oracle={.instructions=expected,.instruction_count=17};
            CHECK(effect_body_records_match(&read,&body,&oracle)==XR_XIR_OK);
            for(uint32_t i=0;i<17;++i){XrXirInstruction actual;
                CHECK(effect_body_instruction_get(&read,&body,i,&actual)==XR_XIR_OK && conditional_compact_same(&actual,&expected[i]));}
            const CompileAllocation *allocation=(const CompileAllocation *)body.runs-1;
            CHECK(allocation->info.resources==context.resources && allocation->info.bytes==3*sizeof(*body.runs));}
        xr_compile_resources_free(body.runs);rp_owner_free(&context,baseline);rp_balanced(physical);
    }
    RootParameterMark physical=rp_mark();XrXirCompileContext context=rp_owner(rp_caps());uint64_t baseline=rp_stats(&context).live_bytes;
    for(uint32_t row=0;row<17;++row){XrXirInstruction source[17];literal_block_values(source);source[row].op=XR_XIR_INVALID;
        XrXirFunction input={.instructions=source,.instruction_count=17};EffectBodyFunction body={.instruction_count=17};
        size_t before=rp_attempts;CHECK(effect_body_records_copy(&context,&input,&body)==XR_XIR_BAD_STRUCTURE &&
            !body.runs && !body.run_capacity && !body.run_count && rp_attempts==before);}
    XrXirInstruction source[17];literal_block_values(source);XrXirFunction input={.instructions=source,.instruction_count=17};
    EffectBodyFunction body={.instruction_count=17};CHECK(effect_body_records_copy(&context,&input,&body)==XR_XIR_OK);
    EffectBodyFunction held=body;XrCompileResourceStats before=rp_stats(&context);
    CHECK(effect_body_records_copy(&context,&input,&body)==XR_XIR_BAD_STRUCTURE && !memcmp(&body,&held,sizeof(body)));
    XrCompileResourceStats after=rp_stats(&context);CHECK(!memcmp(&before,&after,sizeof(before)));
    xr_compile_resources_free(body.runs);rp_owner_free(&context,baseline);rp_balanced(physical);
    fprintf(stderr,"Exact physical census: all17 invalid opcodes refuse before allocation, current invalid/changed-count source refuses after one exact allocation without growth, valid current field mutation survives producer death/full170-word oracle, occupied receiving unchanged; original semantic span/EOF and actual FI/same-ledger controls retained PASS\n");
}
#endif // XIR_SNAPSHOT_EXACT_WRITER_CASES_H
