/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_local_errors_transfer.inc.c - Local error value and cell transfer
 */
static void local_error_cells(LocalErrorCfg *cfg,uint64_t *row,uint32_t target,const uint64_t *value) {
    for (uint32_t v=0;v<cfg->values;++v) {
        if (!xr_xir_type_is_cell(&cfg->terms.types,cfg->types[v])) continue;
        uint64_t *set=local_error_value(cfg,row,v);if (!set) continue;
        memset(set,0,(size_t)cfg->words*sizeof(*set));
        if (target<cfg->values && cfg->roots[v]==cfg->roots[target])
            memcpy(set,value,(size_t)cfg->words*sizeof(*set));
        else local_error_type(cfg,cfg->types[v],set);
    }
}
static void local_error_assign(LocalErrorCfg *cfg,uint64_t *row,uint32_t target,const uint64_t *value) {
    for (uint32_t v=0;v<cfg->values;++v) {
        if (cfg->roots[v]!=cfg->roots[target]) continue;
        uint64_t *set=local_error_value(cfg,row,v);
        if (set) memcpy(set,value,(size_t)cfg->words*sizeof(*set));
    }
}
static bool local_error_boundary(XrXirOp op) {
    return op==XR_XIR_CALL || op==XR_XIR_CALL_DEFAULT || op==XR_XIR_CALL_INDIRECT || op==XR_XIR_CALL_REQUIREMENT ||
        op==XR_XIR_INVOKE || op==XR_XIR_INVOKE_DEFAULT || op==XR_XIR_INVOKE_INDIRECT ||
        op==XR_XIR_TASK_POLL || op==XR_XIR_TASK_AWAIT_RESULT || op==XR_XIR_GO || op==XR_XIR_GENERATOR_NEW || op==XR_XIR_TASK_AWAIT ||
        op==XR_XIR_ITERATOR_HAS_NEXT || op==XR_XIR_ITERATOR_NEXT || op==XR_XIR_ITERATOR_NTH ||
        op==XR_XIR_ITERATOR_CLOSE || op==XR_XIR_SUSPEND || op==XR_XIR_TIMER_AFTER_MS ||
        op==XR_XIR_PRINT || op==XR_XIR_OUTPUT || op==XR_XIR_WRITE_STREAM ||
        op==XR_XIR_CHANNEL_SEND || op==XR_XIR_CHANNEL_RECV || op==XR_XIR_CHANNEL_RECV_OR ||
        op==XR_XIR_CHANNEL_SEND_TIMEOUT || op==XR_XIR_CHANNEL_RECV_TIMEOUT ||
        op==XR_XIR_CLEANUP_LEAVE || op==XR_XIR_CLEANUP_ERROR || op==XR_XIR_CELL_LOCAL_WRITE;
}
static XrXirStatus local_error_instruction(LocalErrorCfg *cfg,uint32_t index) {
    const XrXirInstruction *op=&cfg->operations[index];cfg->instruction=index;
    uint64_t *out=local_error_value(cfg,cfg->row,cfg->head.parameter_count+index);
    if (op->op==XR_XIR_PHI) return XR_XIR_OK;
    if (out) {
        memset(out,0,(size_t)cfg->words*sizeof(*out));
        if (local_error_copy(op->op) || op->op==XR_XIR_CELL_NEW ||
            op->op==XR_XIR_CELL_READ || op->op==XR_XIR_LOCAL_NEW || op->op==XR_XIR_LOCAL_READ ||
            op->op==XR_XIR_OWNED_LOCAL_NEW || op->op==XR_XIR_OWNED_LOCAL_READ ||
            op->op==XR_XIR_SCALAR_LOCAL_NEW || op->op==XR_XIR_SCALAR_LOCAL_READ) {
            memcpy(out,local_error_read(cfg,cfg->row,op->args[0]),(size_t)cfg->words*sizeof(*out));
            if (op->op==XR_XIR_ERROR_NARROW) {
                if (out[0]&1) local_error_type(cfg,op->type,out);
                out[0]&=~UINT64_C(1);
                for (uint32_t a=0;a<cfg->atom_count;++a)
                    if (cfg->atoms[a].type!=op->type) out[(a+1)/64]&=~(UINT64_C(1)<<((a+1)%64));
            }
        } else if (op->op==XR_XIR_ENUM_NEW) {
            for (uint32_t a=0;a<cfg->atom_count;++a)
                if (cfg->atoms[a].type==op->type && cfg->atoms[a].variant==(uint64_t)op->immediate)
                    out[(a+1)/64]|=UINT64_C(1)<<((a+1)%64);
        } else if (op->op==XR_XIR_INVOKE_ERROR) {
            if (op->immediate<0 || (uint64_t)op->immediate>=cfg->head.instruction_count) return XR_XIR_BAD_STRUCTURE;
            local_error_type(cfg,cfg->calls[(uint32_t)op->immediate],out);
        } else if (op->op==XR_XIR_ENUM_GET && op->args[0]<cfg->values) {
            XrXirTaskResultType outcome={0};
            if (op->immediate==XR_XIR_TASK_RESULT_ERROR && !op->args[1] &&
                xr_xir_task_result_type_info(&cfg->terms.types,cfg->types[op->args[0]],&outcome))
                local_error_type(cfg,outcome.error,out);
            else local_error_type(cfg,op->type,out);
        } else local_error_type(cfg,op->type,out);
    }
    if (op->op==XR_XIR_ENUM_NEW && op->immediate==XR_XIR_TASK_RESULT_ERROR) {
        XrXirTaskResultType outcome={0};
        if (xr_xir_task_result_type_info(&cfg->terms.types,op->type,&outcome)) {
            if (op->args[1]!=1 || op->args[0]>=cfg->head.operand_count) return XR_XIR_BAD_STRUCTURE;
            uint32_t value=cfg->head.operands[op->args[0]];
            if (value>=cfg->values || outcome.error==XR_XIR_NEVER) return XR_XIR_BAD_TYPE;
            XrXirStatus status=local_error_subset_bound(cfg,local_error_read(cfg,cfg->row,value),outcome.error);
            if (status!=XR_XIR_OK) return status;
        }
    }
    if (op->op==XR_XIR_LOCAL_WRITE || op->op==XR_XIR_OWNED_LOCAL_WRITE || op->op==XR_XIR_SCALAR_LOCAL_WRITE) {
        if (op->args[0]>=cfg->values) return XR_XIR_BAD_STRUCTURE;
        memcpy(cfg->temporary,local_error_read(cfg,cfg->row,op->args[1]),(size_t)cfg->words*sizeof(uint64_t));
        local_error_assign(cfg,cfg->row,op->args[0],cfg->temporary);
    } else if (op->op==XR_XIR_CELL_WRITE) {
        memcpy(cfg->temporary,local_error_read(cfg,cfg->row,op->args[1]),(size_t)cfg->words*sizeof(uint64_t));
        local_error_cells(cfg,cfg->row,op->args[0],cfg->temporary);
    }
    if (op->op==XR_XIR_THROW)
        return local_error_subset(cfg,local_error_read(cfg,cfg->row,op->args[0]));
    if (op->op==XR_XIR_CALL || op->op==XR_XIR_CALL_DEFAULT || op->op==XR_XIR_CALL_INDIRECT || op->op==XR_XIR_CALL_REQUIREMENT) {
        memset(cfg->temporary,0,(size_t)cfg->words*sizeof(uint64_t));
        local_error_type(cfg,cfg->calls[index],cfg->temporary);
        XrXirStatus status=local_error_subset(cfg,cfg->temporary);if (status!=XR_XIR_OK) return status;
    }
    if (local_error_boundary(op->op)) local_error_cells(cfg,cfg->row,UINT32_MAX,NULL);
    return XR_XIR_OK;
}
static const XrXirInstruction *local_error_definition(LocalErrorCfg *cfg,uint32_t value) {
    return value>=cfg->head.parameter_count && value<cfg->values ?
        &cfg->operations[value-cfg->head.parameter_count]:NULL;
}
static bool local_error_filter(LocalErrorCfg *cfg,const XrXirInstruction *branch,bool yes) {
    const XrXirInstruction *test=local_error_definition(cfg,branch->args[0]);
    if (!test) return true;
    if (test->op==XR_XIR_CONST_BOOL) return (test->immediate!=0)==yes;
    uint32_t value;XrXirType type;int64_t variant=-1;
    if (test->op==XR_XIR_ERROR_IS) { value=test->args[0];type=(XrXirType)test->immediate; }
    else if (test->op==XR_XIR_EQ_INT || test->op==XR_XIR_NE_INT) {
        const XrXirInstruction *tag=local_error_definition(cfg,test->args[0]);
        const XrXirInstruction *constant=local_error_definition(cfg,test->args[1]);
        if (!tag || !constant) return true;
        if (tag->op==XR_XIR_CONST_INT) { const XrXirInstruction *swap=tag;tag=constant;constant=swap; }
        if (tag->op!=XR_XIR_ENUM_TAG || constant->op!=XR_XIR_CONST_INT) return true;
        value=tag->args[0];if (value>=cfg->values) return false;
        type=cfg->types[value];variant=constant->immediate;
        if (test->op==XR_XIR_NE_INT) yes=!yes;
        if (variant<0) return !yes;
    } else return true;
    if (value>=cfg->values || cfg->slots[value]==UINT32_MAX) return true;
    for (uint32_t v=0;v<cfg->values;++v) {
        if (cfg->roots[v]!=cfg->roots[value]) continue;
        uint64_t *set=local_error_value(cfg,cfg->edge,v);if (!set) continue;
        if (yes && (set[0]&1)) { set[0]&=~UINT64_C(1);local_error_type(cfg,type,set); }
        for (uint32_t a=0;a<cfg->atom_count;++a) {
            bool match=cfg->atoms[a].type==type && (variant<0 || cfg->atoms[a].variant==(uint64_t)variant);
            if (match!=yes) set[(a+1)/64]&=~(UINT64_C(1)<<((a+1)%64));
        }
    }
    return local_error_any(local_error_read(cfg,cfg->edge,value),cfg->words);
}
