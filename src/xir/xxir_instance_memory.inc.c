/* Governed memory calls share one typed path in VM and generated C. */
#include "../shared/xr_sync_core.h"
static XrXirValueStatus instance_memory_call(XrXirInstance *instance,XrXirCallView *view,
    uint32_t id,XrXirType result,XrXirValue *output) {
    const XrXirValue *args=view->arguments;XrXirValueStatus status=XR_XIR_VALUE_OK;
    int64_t integer=0;bool boolean=false;
    switch(id) {
    case XR_NATIVE_CALLABLE_BUFFER_ALLOCATE:
    case XR_NATIVE_CALLABLE_BUFFER_ALLOCATE_ZEROED:
    case XR_NATIVE_CALLABLE_BUFFER_ALLOCATE_ALIGNED: {
        int64_t alignment=id==XR_NATIVE_CALLABLE_BUFFER_ALLOCATE_ALIGNED?args[1].payload:0;
        if (id==XR_NATIVE_CALLABLE_BUFFER_ALLOCATE_ALIGNED &&
            (alignment<(int64_t)sizeof(void *) || (alignment&(alignment-1)))) return XR_XIR_VALUE_BAD_ARGUMENT;
        return xr_xir_buffer_storage_new(instance->domain,args[0].payload,alignment,
            id==XR_NATIVE_CALLABLE_BUFFER_ALLOCATE_ZEROED,output);
    }
    case XR_NATIVE_CALLABLE_BUFFER_LENGTH:
        status=xr_xir_buffer_storage_length(&args[0],&integer);break;
    case XR_NATIVE_CALLABLE_BUFFER_AS_BYTES:
        return xr_xir_call_buffer_view(view,result,output);
    case XR_NATIVE_CALLABLE_BUFFER_BORROW_PTR:
        return xr_xir_buffer_storage_pointer(&args[0],view->arena,result,output);
    case XR_NATIVE_CALLABLE_BUFFER_RESIZE:
        status=xr_xir_buffer_storage_resize(&args[0],args[1].payload,&boolean);break;
    case XR_NATIVE_CALLABLE_MEM_FENCE:
        xr_sync_core_fence(args[0].payload);return XR_XIR_VALUE_OK;
    case XR_NATIVE_CALLABLE_MEM_PREFETCH:
        return xr_xir_pointer_prefetch(&args[0],args[1].payload);
    case XR_NATIVE_CALLABLE_MEM_CACHE_FLUSH:
        return xr_xir_pointer_cache_flush(&args[0],args[1].payload);
    case XR_NATIVE_CALLABLE_MEM_NONTEMPORAL_STORE:
        return xr_xir_pointer_volatile_store(&args[0],args[1].payload,args[2].payload,true);
    case XR_NATIVE_CALLABLE_MEM_CACHE_LINE_SIZE:
        integer=64;break;
    case XR_NATIVE_CALLABLE_MEM_PAGE_ALLOCATE:
        return xr_xir_page_allocate(instance->domain,view->arena,result,args[0].payload,args[1].payload,output);
    case XR_NATIVE_CALLABLE_MEM_PAGE_PROTECT:
        status=xr_xir_page_protect(&args[0],args[1].payload,args[2].payload,&boolean);break;
    case XR_NATIVE_CALLABLE_MEM_PAGE_FREE:
        status=xr_xir_page_free(&args[0],args[1].payload,&boolean);break;
    case XR_NATIVE_CALLABLE_MEM_MOVE:
        return xr_xir_pointer_copy(&args[0],&args[1],args[2].payload,true);
    case XR_NATIVE_CALLABLE_MEM_VOLATILE_LOAD:
        status=xr_xir_pointer_volatile_load(&args[0],args[1].payload,&integer);break;
    case XR_NATIVE_CALLABLE_MEM_VOLATILE_STORE:
        return xr_xir_pointer_volatile_store(&args[0],args[1].payload,args[2].payload,false);
    default:return XR_XIR_VALUE_BAD_ARGUMENT;
    }
    if (status==XR_XIR_VALUE_OK) *output=(XrXirValue){result,0,result==XR_XIR_BOOL?(int64_t)boolean:integer};
    return status;
}
