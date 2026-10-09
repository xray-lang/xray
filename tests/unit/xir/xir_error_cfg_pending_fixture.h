/* Independent CFGs reuse qualified enum declarations, not current run results. */
#ifndef XIR_ERROR_CFG_PENDING_FIXTURE_H
#define XIR_ERROR_CFG_PENDING_FIXTURE_H
#include "xir_error_edge_snapshot_fixture.h"
typedef enum PendingCase {
    PC_FORWARD, PC_REVERSE, PC_PHI_SWAP, PC_PHI_SELF, PC_LATE_JOIN,
    PC_FILTER, PC_INVOKE, PC_CLEANUP, PC_PANIC, PC_TEMPORARY
} PendingCase;
static uint32_t pc_remap_value(const XrXirFunction *f,uint32_t value,const uint32_t *indices) {
    CHECK(value<f->parameter_count+f->instruction_count);
    return value<f->parameter_count ? value : f->parameter_count+indices[value-f->parameter_count];
}
static void pc_reverse_blocks(EdgeFixture *f) {
    const XrXirFunction *function=&f->functions[1];
    uint32_t n=function->block_count,cursor=0;
    XrXirBlock saved[EE_BLOCKS];XrXirInstruction instructions[EE_MAX_OPS];
    uint32_t indices[EE_MAX_OPS];
    memcpy(saved,f->blocks,(size_t)n*sizeof(*saved));
    memcpy(instructions,f->subject,(size_t)function->instruction_count*sizeof(*instructions));
    /* Block storage and instruction ranges remain canonical after renumbering. */
    for (uint32_t b=0;b<n;++b) {
        uint32_t original=b ? n-b : 0;
        f->blocks[b]=saved[original];f->blocks[b].first=cursor;
        for (uint32_t i=saved[original].first;i<saved[original].first+saved[original].count;++i) {
            indices[i]=cursor;f->subject[cursor++]=instructions[i];
        }
    }
    CHECK(cursor==function->instruction_count);
    for (uint32_t b=0;b<n;++b) {
        if (f->blocks[b].panic) f->blocks[b].panic=n-f->blocks[b].panic;
        if (f->blocks[b].frontier) f->blocks[b].frontier=indices[f->blocks[b].frontier-1]+1;
    }
    for (uint32_t i=0;i<function->instruction_count;++i) {
        XrXirInstruction *op=&f->subject[i];
        unsigned values=op->op==XR_XIR_COPY || op->op==XR_XIR_CELL_NEW || op->op==XR_XIR_CELL_READ ||
            op->op==XR_XIR_ENUM_TAG || op->op==XR_XIR_THROW || op->op==XR_XIR_BRANCH ? 1u :
            op->op==XR_XIR_CELL_WRITE || op->op==XR_XIR_EQ_INT ? 2u : 0u;
        for (unsigned v=0;v<values;++v) op->args[v]=pc_remap_value(function,op->args[v],indices);
        if (op->op==XR_XIR_PHI) {
            for (uint32_t p=0;p<op->args[1];p+=2) {
                uint32_t offset=op->args[0]+p;
                if (f->operands[offset]) f->operands[offset]=n-f->operands[offset];
                f->operands[offset+1]=pc_remap_value(function,f->operands[offset+1],indices);
            }
        } else if (op->op==XR_XIR_CLEANUP_REGISTER || op->op==XR_XIR_CALL || op->op==XR_XIR_INVOKE)
            for (uint32_t p=0;p<op->args[1];++p)
                f->operands[op->args[0]+p]=pc_remap_value(function,f->operands[op->args[0]+p],indices);
        if (op->op==XR_XIR_INVOKE_ERROR) {
            CHECK(op->immediate>=0 && (uint64_t)op->immediate<function->instruction_count);
            op->immediate=indices[(uint32_t)op->immediate];
        }
        unsigned targets=op->op==XR_XIR_BRANCH || op->op==XR_XIR_INVOKE ? 2u :
            op->op==XR_XIR_JUMP || op->op==XR_XIR_CLEANUP_REGISTER || op->op==XR_XIR_CLEANUP_LEAVE ? 1u : 0u;
        for (unsigned target=0;target<targets;++target)
            if (op->targets[target]) op->targets[target]=n-op->targets[target];
    }
}
static void pc_fixture(EdgeFixture *f,PendingCase mode,bool snapshot) {
    EdgeCase original=mode==PC_FORWARD || mode==PC_REVERSE ? EE_BULK :
        mode==PC_PHI_SWAP || mode==PC_PHI_SELF || mode==PC_LATE_JOIN ? EE_PHI :
        mode==PC_FILTER ? EE_FILTER : mode==PC_INVOKE ? EE_INVOKE :
        mode==PC_CLEANUP ? EE_CLEANUP : mode==PC_PANIC ? EE_PANIC : EE_TEMPORARY;
    ee_fixture(f,original,snapshot);
    if (mode==PC_PHI_SELF) {
        static const uint32_t pairs[8]={0,2,1,6,0,3,1,5};
        memcpy(f->operands,pairs,sizeof(pairs));
        f->subject[5].targets[0]=1;f->subject[5].targets[1]=2;
        f->subject[6]=(XrXirInstruction){.op=XR_XIR_THROW,.args={5}};
        f->blocks[2]=(XrXirBlock){.first=6,.count=1};
        f->functions[1].block_count=3;f->functions[1].instruction_count=7;
    } else if (mode==PC_LATE_JOIN) {
        /* Consumer 2 runs before producer 3 supplies its distinct enum arm. */
        static const uint32_t pairs[4]={1,2,3,3};
        memcpy(f->operands,pairs,sizeof(pairs));
        f->subject[2]=(XrXirInstruction){.op=XR_XIR_BRANCH,.args={1},.targets={1,3}};
        f->subject[3]=(XrXirInstruction){.op=XR_XIR_JUMP,.targets={2}};
        f->subject[4]=(XrXirInstruction){.op=XR_XIR_PHI,.type=(XrXirType)EE_ENUM,.args={0,4}};
        f->subject[5]=(XrXirInstruction){.op=XR_XIR_THROW,.args={6}};
        f->subject[6]=f->subject[3];
        f->blocks[1]=(XrXirBlock){.first=3,.count=1};
        f->blocks[2]=(XrXirBlock){.first=4,.count=2};
        f->blocks[3]=(XrXirBlock){.first=6,.count=1};
        f->functions[1].instruction_count=7;f->functions[1].operand_count=4;
    } else if (mode!=PC_FORWARD && mode!=PC_TEMPORARY) pc_reverse_blocks(f);
}
static uint64_t pc_expected(PendingCase mode,bool snapshot) {
    if (mode==PC_PHI_SWAP || mode==PC_PHI_SELF || mode==PC_LATE_JOIN || mode==PC_INVOKE) return 12;
    if (mode==PC_CLEANUP || mode==PC_PANIC) return snapshot ? 4u : 12u;
    if (mode==PC_TEMPORARY) return snapshot ? 4u : 8u;
    /* The reused declaration fixture's original constants remain independent. */
    CHECK(ee_expected(EE_BULK,false)==4);
    return 4;
}
#endif
