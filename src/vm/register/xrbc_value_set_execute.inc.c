/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xrbc_value_set_execute.inc.c - Fixed Set owners and common binding updates
 */
static XrtManagedStatus rbc_value_set_step(const XrRbcModule *module,XrRbcStack *stack,
    RbcValueActivation *activation,XrRbcValueAccess access,uint32_t source) {
    const uint32_t *args=access.count ? activation->code->arguments+access.first : NULL;
    bool mutate=access.index==XR_LIR_SET_ADD || access.index==XR_LIR_SET_REMOVE || access.index==XR_LIR_SET_CLEAR;
    const XrtBackingTypes *types=NULL;
    if (access.index==XR_LIR_SET_NEW || mutate) {
        XrtManagedStatus status=xrt_native_values_backings(xrt_value_root_current(source),&types);
        if (status!=XRT_MANAGED_OK) return status;
    }
    if (access.index==XR_LIR_SET_NEW) {
        if (access.count) return XRT_MANAGED_ARGUMENT;
        const XrLirValueHome *home=rbc_value_home(activation->code,access.result);
        XrtManagedStatus status=xrt_set_backing_create(activation->frame,home->temporary_first_owner,types,NULL,0);
        return status==XRT_MANAGED_OK ? rbc_value_publish_leaf(activation,access.result) : status;
    }
    if (mutate) {
        XrtExternalOwner external; XrtPlace binding;
        XrtManagedStatus status=rbc_value_binding_place(module,stack,rbc_value_origin(activation,args[0]),&external,&binding);
        if (status!=XRT_MANAGED_OK) return status;
        XrtSetUpdate update={.operation=access.index==XR_LIR_SET_ADD ? XRT_SET_UPDATE_ADD :
            access.index==XR_LIR_SET_REMOVE ? XRT_SET_UPDATE_REMOVE : XRT_SET_UPDATE_CLEAR};
        if (access.count>1) {
            XrRbcRegister *input=rbc_value_register(stack,activation,args[1]);
            if (!input) return XRT_MANAGED_ARGUMENT;
            memcpy(&update.key,&input->bits,sizeof(update.key));
        }
        const XrLirHomeWorkspace *workspace=&activation->code->homes->workspaces[access.window];
        bool removed=false;
        status=xrt_set_update_place(binding,activation->frame,workspace->first_owner,types,&update,&removed);
        if (status==XRT_MANAGED_OK && access.result) {
            XrRbcRegister *result=rbc_value_register(stack,activation,access.result);
            if (!result) return XRT_MANAGED_ARGUMENT;
            result->bits=removed;
        }
        return status;
    }
    const XrtSetBacking *set=NULL;
    XrtManagedStatus status=rbc_value_set_borrow(module,activation,args[0],&set);
    if (status!=XRT_MANAGED_OK) return status;
    if (access.index==XR_LIR_SET_DISPLAY) {
        const XrLirValueHome *home=rbc_value_home(activation->code,access.result);
        xrt_native_set_display(activation->frame,home->temporary_first_owner,&set->managed,source);
        return rbc_value_publish_leaf(activation,access.result);
    }
    XrRbcRegister *result=rbc_value_register(stack,activation,access.result);
    if (!result) return XRT_MANAGED_ARGUMENT;
    if (access.index==XR_LIR_SET_LEN) {
        if (set->length>INT64_MAX) return XRT_MANAGED_OVERFLOW;
        result->bits=set->length; return XRT_MANAGED_OK;
    }
    XrRbcRegister *input=rbc_value_register(stack,activation,args[1]);
    if (!input) return XRT_MANAGED_ARGUMENT;
    int64_t key=0; bool found=false;
    memcpy(&key,&input->bits,sizeof(key));
    status=xrt_set_backing_contains(set,key,&found);
    if (status==XRT_MANAGED_OK) result->bits=found;
    return status;
}
