/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_instance_io.inc.c - Declaration-bound Path reads and owned OS mutations
 *
 * KEY CONCEPT:
 *   The retained Input authorizes a public field read. A temporary ancestor
 *   view preserves the object's actual type, arena and ordinary loan checks.
 */
#include "../os/os_fs.h"
#include "../shared/xr_utf8_core.h"

static XrXirCallStatus instance_io_permission(XrXirCallView *view,XrXirInstance *instance,
    uint32_t index,uint32_t *field) {
    const XrXirModule *module=instance_method_module(instance);
    XrXirValueAdmission *admission=xr_xir_call_admission(view);
    if(!module || !module->types || !module->types->nominals || !module->declarations ||
        index>=module->function_count || !admission || admission->domain!=instance->domain ||
        admission->arena!=view->arena)return XR_XIR_CALL_BAD_STATE;
    const XrXirFunction *function=module->functions+index;
    if(!function->parameters || !function->parameter_count ||
        function->parameter_count!=instance->program->entries[index].parameter_count ||
        function->parameters[0]!=instance->program->entries[index].parameters[0] ||
        !xir_io_native_id(module->declarations->functions[index].native_callable))return XR_XIR_CALL_BAD_STATE;
    const XrXirTypeNode *path=xr_xir_type_node(module->types,function->parameters[0]);
    if(!path || path->kind!=XR_XIR_TYPE_NOMINAL)return XR_XIR_CALL_BAD_STATE;
    uint64_t work=32u+8u*((uint64_t)module->types->nominals->count+
        module->declarations->module_count+path->nominal.field_count);
    if(work>admission->work || !xr_xir_domain_work(instance->domain,work))return XR_XIR_CALL_LIMIT;
    admission->work-=work;
    if(xir_native_callable_verify(module,index)!=XR_XIR_OK ||
        !xir_io_native_scope(module->declarations,module->types,index,module->function_count,function->parameters[0],field) ||
        xr_xir_compile_nominal_access(&instance->program->context,module,index,function->parameters[0],*field,
            XR_XIR_NOMINAL_READ)!=XR_XIR_OK)return XR_XIR_CALL_BAD_STATE;
    return XR_XIR_CALL_READY;
}

static XrXirCallStatus instance_io_call(XrXirCallView *view,XrXirInstance *instance,
    uint32_t index,uint32_t id,const XrXirCallEntry *entry,XrXirValue *output) {
    uint32_t field=0;
    XrXirCallStatus status=instance_io_permission(view,instance,index,&field);
    if(status!=XR_XIR_CALL_READY)return status;
    XrXirValue base={0},text={0};
    /* This borrowed view is never retained or dropped. Its arena and Path
     * ancestor coordinate are recovered from the authenticated actual object. */
    if(!xr_xir_value_from_payload(view->arena,entry->parameters[0],view->arguments[0].payload,&base))
        return XR_XIR_CALL_BAD_ARGUMENT;
    status=value_call_status(xr_xir_class_get(&base,field,xr_xir_call_admission(view),&text));
    if(status!=XR_XIR_CALL_READY)return status;
    const char *bytes=NULL;size_t length=0;bool changed=false;
    if(!xr_xir_string_view(&text,&bytes,&length))status=XR_XIR_CALL_HOST_ERROR;
    if(status==XR_XIR_CALL_READY && (length>INT_MAX || length>SIZE_MAX-1 ||
        !xr_xir_domain_work(instance->domain,length+1)))status=XR_XIR_CALL_LIMIT;
    if(status==XR_XIR_CALL_READY && xr_utf8_core_scan_strict((const uint8_t *)bytes,length).error!=XR_UTF8_OK)
        status=XR_XIR_CALL_HOST_ERROR;
    if(id==XR_NATIVE_CALLABLE_IO_STAT) {
        if(status==XR_XIR_CALL_READY)status=instance_file_metadata_query(view,instance,bytes,length,entry->result,output);
        xr_xir_value_drop(&text);return status;
    }
    bool chmod=id==XR_NATIVE_CALLABLE_IO_CHMOD;
    bool valid=status==XR_XIR_CALL_READY && !memchr(bytes,0,length) &&
        (!chmod || (view->arguments[1].payload>=0 && view->arguments[1].payload<=INT_MAX));
    XirEnvironmentQuery query={view,instance->domain,status};
    void *owned=NULL;XrOsIoStatus called=XR_OS_IO_OK;
    if(valid && instance_environment_current(&query)) {
        XrOsIoPolicy policy={&query,instance_environment_allocate,instance_environment_free};
        called=instance_environment_allocate(&query,length+1,&owned);
        if(called==XR_OS_IO_OK) {
            memcpy(owned,bytes,length);((char *)owned)[length]=0;
            called=chmod?xr_os_io_chmod(&policy,owned,view->arguments[1].payload,instance_environment_current,&changed):
                xr_os_io_chdir(&policy,owned,instance_environment_current,&changed);
        }
    }
    instance_environment_free(&query,owned);xr_xir_value_drop(&text);
    if(!instance_environment_current(&query))return query.status;
    if(valid)status=instance_environment_status(called);
    if(status==XR_XIR_CALL_READY)*output=(XrXirValue){XR_XIR_BOOL,0,changed?1:0};
    return status;
}
