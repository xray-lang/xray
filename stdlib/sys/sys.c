#include "sys.h"

#include "../common.h"
#include "../../src/base/xchecks.h"
#include "../../src/module/xmodule.h"
#include "../../src/os/os_thread.h"
#include "../../src/runtime/class/xclass.h"
#include "../../src/runtime/class/xclass_builder.h"
#include "../../src/runtime/class/xclass_system.h"
#include "../../src/runtime/class/xinstance.h"
#include "../../src/runtime/object/xpanic_info.h"
#include "../../src/runtime/value/xvalue.h"
#include "../../src/runtime/xisolate_api.h"
#include "../../src/runtime/xvm_call.h"
#include "../../src/vm/xvm.h"
#include "../../src/vm/xvm_closure.h"

typedef struct XrSysMutexBody {
    xr_mutex_t mutex;
    bool initialized;
} XrSysMutexBody;

typedef struct XrSysRwLockBody {
    xr_rwlock_t rwlock;
    bool initialized;
} XrSysRwLockBody;

typedef struct XrSysCondvarBody {
    xr_cond_t cond;
    bool initialized;
} XrSysCondvarBody;

typedef struct XrSysBarrierBody {
    xr_mutex_t mutex;
    xr_cond_t cond;
    int64_t parties;
    int64_t arrived;
    int64_t generation;
    bool initialized;
} XrSysBarrierBody;

typedef struct XrSysOnceBody {
    xr_once_t once;
    bool initialized;
} XrSysOnceBody;

static XR_THREAD_LOCAL XrVMRuntime *g_sys_once_isolate = NULL;
static XR_THREAD_LOCAL XrClosure *g_sys_once_body = NULL;

static void sys_once_call_body(void) {
    if (g_sys_once_isolate && g_sys_once_body)
        (void) xr_vm_call_closure(g_sys_once_isolate, g_sys_once_body, NULL, 0);
}

static void sys_mutex_body_init(XrInstance *instance, void *body_ptr) {
    (void) instance;
    XrSysMutexBody *body = (XrSysMutexBody *) body_ptr;
    xr_mutex_init(&body->mutex);
    body->initialized = true;
}

static void sys_mutex_body_destroy(void *body_ptr) {
    XrSysMutexBody *body = (XrSysMutexBody *) body_ptr;
    if (!body || !body->initialized)
        return;
    xr_mutex_destroy(&body->mutex);
    body->initialized = false;
}

static XrNativeBodyDesc g_sys_mutex_body_desc = {
    .body_size = sizeof(XrSysMutexBody),
    .body_align = _Alignof(XrSysMutexBody),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = sys_mutex_body_init,
    .destroy = sys_mutex_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static void sys_rwlock_body_init(XrInstance *instance, void *body_ptr) {
    (void) instance;
    XrSysRwLockBody *body = (XrSysRwLockBody *) body_ptr;
    xr_rwlock_init(&body->rwlock);
    body->initialized = true;
}

static void sys_rwlock_body_destroy(void *body_ptr) {
    XrSysRwLockBody *body = (XrSysRwLockBody *) body_ptr;
    if (!body || !body->initialized)
        return;
    xr_rwlock_destroy(&body->rwlock);
    body->initialized = false;
}

static XrNativeBodyDesc g_sys_rwlock_body_desc = {
    .body_size = sizeof(XrSysRwLockBody),
    .body_align = _Alignof(XrSysRwLockBody),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = sys_rwlock_body_init,
    .destroy = sys_rwlock_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static void sys_condvar_body_init(XrInstance *instance, void *body_ptr) {
    (void) instance;
    XrSysCondvarBody *body = (XrSysCondvarBody *) body_ptr;
    xr_cond_init(&body->cond);
    body->initialized = true;
}

static void sys_condvar_body_destroy(void *body_ptr) {
    XrSysCondvarBody *body = (XrSysCondvarBody *) body_ptr;
    if (!body || !body->initialized)
        return;
    xr_cond_destroy(&body->cond);
    body->initialized = false;
}

static XrNativeBodyDesc g_sys_condvar_body_desc = {
    .body_size = sizeof(XrSysCondvarBody),
    .body_align = _Alignof(XrSysCondvarBody),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = sys_condvar_body_init,
    .destroy = sys_condvar_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static void sys_barrier_body_init(XrInstance *instance, void *body_ptr) {
    (void) instance;
    XrSysBarrierBody *body = (XrSysBarrierBody *) body_ptr;
    xr_mutex_init(&body->mutex);
    xr_cond_init(&body->cond);
    body->parties = 1;
    body->arrived = 0;
    body->generation = 0;
    body->initialized = true;
}

static void sys_barrier_body_destroy(void *body_ptr) {
    XrSysBarrierBody *body = (XrSysBarrierBody *) body_ptr;
    if (!body || !body->initialized)
        return;
    xr_cond_destroy(&body->cond);
    xr_mutex_destroy(&body->mutex);
    body->initialized = false;
}

static XrNativeBodyDesc g_sys_barrier_body_desc = {
    .body_size = sizeof(XrSysBarrierBody),
    .body_align = _Alignof(XrSysBarrierBody),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = sys_barrier_body_init,
    .destroy = sys_barrier_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static void sys_once_body_init(XrInstance *instance, void *body_ptr) {
    (void) instance;
    XrSysOnceBody *body = (XrSysOnceBody *) body_ptr;
    xr_once_t init = XR_ONCE_INITIALIZER;
    body->once = init;
    body->initialized = true;
}

static void sys_once_body_destroy(void *body_ptr) {
    XrSysOnceBody *body = (XrSysOnceBody *) body_ptr;
    if (body)
        body->initialized = false;
}

static XrNativeBodyDesc g_sys_once_body_desc = {
    .body_size = sizeof(XrSysOnceBody),
    .body_align = _Alignof(XrSysOnceBody),
    .copy_policy = XR_NATIVE_BODY_COPY_FORBID,
    .init = sys_once_body_init,
    .destroy = sys_once_body_destroy,
    .deep_copy = NULL,
    .to_shared = NULL,
};

static XrClass *sys_mutex_class(XrVMRuntime *isolate) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->sysMutexClass != NULL, "sys.Mutex class not registered");
    return core ? core->sysMutexClass : NULL;
}

static XrClass *sys_rwlock_class(XrVMRuntime *isolate) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->sysRwLockClass != NULL, "sys.RwLock class not registered");
    return core ? core->sysRwLockClass : NULL;
}

static XrClass *sys_condvar_class(XrVMRuntime *isolate) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->sysCondvarClass != NULL, "sys.Condvar class not registered");
    return core ? core->sysCondvarClass : NULL;
}

static XrClass *sys_barrier_class(XrVMRuntime *isolate) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->sysBarrierClass != NULL, "sys.Barrier class not registered");
    return core ? core->sysBarrierClass : NULL;
}

static XrClass *sys_once_class(XrVMRuntime *isolate) {
    XrayCoreClasses *core = xr_isolate_get_core_classes(isolate);
    XR_DCHECK(core != NULL && core->sysOnceClass != NULL, "sys.Once class not registered");
    return core ? core->sysOnceClass : NULL;
}

static XrSysMutexBody *sys_mutex_body(XrVMRuntime *isolate, XrValue self) {
    if (!XR_IS_INSTANCE(self))
        return NULL;
    XrInstance *instance = (XrInstance *) XR_TO_PTR(self);
    XrClass *klass = sys_mutex_class(isolate);
    if (!klass || !xr_class_instanceof(instance->klass, klass))
        return NULL;
    return (XrSysMutexBody *) xr_instance_native_body(instance);
}

static XrSysRwLockBody *sys_rwlock_body(XrVMRuntime *isolate, XrValue self) {
    if (!XR_IS_INSTANCE(self))
        return NULL;
    XrInstance *instance = (XrInstance *) XR_TO_PTR(self);
    XrClass *klass = sys_rwlock_class(isolate);
    if (!klass || !xr_class_instanceof(instance->klass, klass))
        return NULL;
    return (XrSysRwLockBody *) xr_instance_native_body(instance);
}

static XrSysCondvarBody *sys_condvar_body(XrVMRuntime *isolate, XrValue self) {
    if (!XR_IS_INSTANCE(self))
        return NULL;
    XrInstance *instance = (XrInstance *) XR_TO_PTR(self);
    XrClass *klass = sys_condvar_class(isolate);
    if (!klass || !xr_class_instanceof(instance->klass, klass))
        return NULL;
    return (XrSysCondvarBody *) xr_instance_native_body(instance);
}

static XrSysBarrierBody *sys_barrier_body(XrVMRuntime *isolate, XrValue self) {
    if (!XR_IS_INSTANCE(self))
        return NULL;
    XrInstance *instance = (XrInstance *) XR_TO_PTR(self);
    XrClass *klass = sys_barrier_class(isolate);
    if (!klass || !xr_class_instanceof(instance->klass, klass))
        return NULL;
    return (XrSysBarrierBody *) xr_instance_native_body(instance);
}

static XrSysOnceBody *sys_once_body(XrVMRuntime *isolate, XrValue self) {
    if (!XR_IS_INSTANCE(self))
        return NULL;
    XrInstance *instance = (XrInstance *) XR_TO_PTR(self);
    XrClass *klass = sys_once_class(isolate);
    if (!klass || !xr_class_instanceof(instance->klass, klass))
        return NULL;
    return (XrSysOnceBody *) xr_instance_native_body(instance);
}

static XrValue sys_mutex_invalid_receiver(XrVMRuntime *isolate) {
    XrValue exc = xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH,
                                     "sys.Mutex method called with non-Mutex receiver");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_rwlock_invalid_receiver(XrVMRuntime *isolate) {
    XrValue exc = xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH,
                                     "sys.RwLock method called with non-RwLock receiver");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_condvar_invalid_receiver(XrVMRuntime *isolate) {
    XrValue exc = xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH,
                                     "sys.Condvar method called with non-Condvar receiver");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_condvar_invalid_mutex(XrVMRuntime *isolate) {
    XrValue exc =
        xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH, "sys.Condvar requires a sys.Mutex");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_barrier_invalid_receiver(XrVMRuntime *isolate) {
    XrValue exc = xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH,
                                     "sys.Barrier method called with non-Barrier receiver");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_barrier_invalid_parties(XrVMRuntime *isolate) {
    XrValue exc =
        xr_panic_info_newf(isolate, XR_ERR_INVALID_ARG_TYPE, "sys.Barrier parties must be > 0");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_once_invalid_receiver(XrVMRuntime *isolate) {
    XrValue exc = xr_panic_info_newf(isolate, XR_ERR_TYPE_MISMATCH,
                                     "sys.Once method called with non-Once receiver");
    xr_vm_throw_exception(isolate, exc);
    return xr_null();
}

static XrValue sys_mutex_new(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrInstance *instance = xr_instance_new(isolate, sys_mutex_class(isolate));
    if (!instance) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "sys.Mutex allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_instance(instance);
}

static XrValue sys_rwlock_new(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrInstance *instance = xr_instance_new(isolate, sys_rwlock_class(isolate));
    if (!instance) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "sys.RwLock allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_instance(instance);
}

static XrValue sys_condvar_new(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;

    XrInstance *instance = xr_instance_new(isolate, sys_condvar_class(isolate));
    if (!instance) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "sys.Condvar allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_instance(instance);
}

static XrValue sys_barrier_new(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) argc;
    if (!XR_IS_INT(args[0]) || XR_TO_INT(args[0]) <= 0)
        return sys_barrier_invalid_parties(isolate);

    XrInstance *instance = xr_instance_new(isolate, sys_barrier_class(isolate));
    if (!instance) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "sys.Barrier allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    XrValue value = xr_value_from_instance(instance);
    XrSysBarrierBody *body = sys_barrier_body(isolate, value);
    if (body)
        body->parties = XR_TO_INT(args[0]);
    return value;
}

static XrValue sys_once_new(XrVMRuntime *isolate, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrInstance *instance = xr_instance_new(isolate, sys_once_class(isolate));
    if (!instance) {
        XrValue exc =
            xr_panic_info_newf(isolate, XR_ERR_OUT_OF_MEMORY, "sys.Once allocation failed");
        xr_vm_throw_exception(isolate, exc);
        return xr_null();
    }
    return xr_value_from_instance(instance);
}

static XrValue sys_mutex_lock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysMutexBody *body = sys_mutex_body(isolate, self);
    if (!body)
        return sys_mutex_invalid_receiver(isolate);
    xr_mutex_lock(&body->mutex);
    return xr_null();
}

static XrValue sys_mutex_unlock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysMutexBody *body = sys_mutex_body(isolate, self);
    if (!body)
        return sys_mutex_invalid_receiver(isolate);
    xr_mutex_unlock(&body->mutex);
    return xr_null();
}

static XrValue sys_mutex_try_lock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysMutexBody *body = sys_mutex_body(isolate, self);
    if (!body)
        return sys_mutex_invalid_receiver(isolate);
    return xr_bool(xr_mutex_trylock(&body->mutex));
}

static XrValue sys_rwlock_rdlock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysRwLockBody *body = sys_rwlock_body(isolate, self);
    if (!body)
        return sys_rwlock_invalid_receiver(isolate);
    xr_rwlock_rdlock(&body->rwlock);
    return xr_null();
}

static XrValue sys_rwlock_rdunlock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysRwLockBody *body = sys_rwlock_body(isolate, self);
    if (!body)
        return sys_rwlock_invalid_receiver(isolate);
    xr_rwlock_rdunlock(&body->rwlock);
    return xr_null();
}

static XrValue sys_rwlock_wrlock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysRwLockBody *body = sys_rwlock_body(isolate, self);
    if (!body)
        return sys_rwlock_invalid_receiver(isolate);
    xr_rwlock_wrlock(&body->rwlock);
    return xr_null();
}

static XrValue sys_rwlock_wrunlock(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysRwLockBody *body = sys_rwlock_body(isolate, self);
    if (!body)
        return sys_rwlock_invalid_receiver(isolate);
    xr_rwlock_wrunlock(&body->rwlock);
    return xr_null();
}

static XrValue sys_condvar_wait(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) argc;
    XrSysCondvarBody *body = sys_condvar_body(isolate, self);
    if (!body)
        return sys_condvar_invalid_receiver(isolate);
    XrSysMutexBody *mutex = sys_mutex_body(isolate, args[0]);
    if (!mutex)
        return sys_condvar_invalid_mutex(isolate);
    xr_cond_wait(&body->cond, &mutex->mutex);
    return xr_null();
}

static XrValue sys_condvar_wait_for(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) argc;
    XrSysCondvarBody *body = sys_condvar_body(isolate, self);
    if (!body)
        return sys_condvar_invalid_receiver(isolate);
    XrSysMutexBody *mutex = sys_mutex_body(isolate, args[0]);
    if (!mutex)
        return sys_condvar_invalid_mutex(isolate);
    uint64_t timeout_ns =
        XR_IS_INT(args[1]) && XR_TO_INT(args[1]) > 0 ? (uint64_t) XR_TO_INT(args[1]) : 0u;
    return xr_bool(xr_cond_wait_for_ns(&body->cond, &mutex->mutex, timeout_ns));
}

static XrValue sys_condvar_signal(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysCondvarBody *body = sys_condvar_body(isolate, self);
    if (!body)
        return sys_condvar_invalid_receiver(isolate);
    xr_cond_signal(&body->cond);
    return xr_null();
}

static XrValue sys_condvar_broadcast(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysCondvarBody *body = sys_condvar_body(isolate, self);
    if (!body)
        return sys_condvar_invalid_receiver(isolate);
    xr_cond_broadcast(&body->cond);
    return xr_null();
}

static XrValue sys_barrier_wait(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) args;
    (void) argc;
    XrSysBarrierBody *body = sys_barrier_body(isolate, self);
    if (!body)
        return sys_barrier_invalid_receiver(isolate);

    xr_mutex_lock(&body->mutex);
    int64_t generation = body->generation;
    body->arrived++;
    if (body->arrived >= body->parties) {
        body->arrived = 0;
        body->generation++;
        xr_cond_broadcast(&body->cond);
        xr_mutex_unlock(&body->mutex);
        return xr_bool(true);
    }
    while (generation == body->generation)
        xr_cond_wait(&body->cond, &body->mutex);
    xr_mutex_unlock(&body->mutex);
    return xr_bool(true);
}

static XrValue sys_once_call(XrVMRuntime *isolate, XrValue self, XrValue *args, int argc) {
    (void) argc;
    XrSysOnceBody *body = sys_once_body(isolate, self);
    if (!body)
        return sys_once_invalid_receiver(isolate);

    XrClosure *closure = xr_vm_closure_from_arg(isolate, args[0], "sys.Once.call");
    if (!closure)
        return xr_null();

    XrVMRuntime *prev_isolate = g_sys_once_isolate;
    XrClosure *prev_body = g_sys_once_body;
    g_sys_once_isolate = isolate;
    g_sys_once_body = closure;
    xr_once_call(&body->once, sys_once_call_body);
    g_sys_once_isolate = prev_isolate;
    g_sys_once_body = prev_body;
    return xr_null();
}

#define XR_STDLIB_VM_BIND_CLASS_CONDVAR 1
#define XR_STDLIB_VM_BIND_CLASS_BARRIER 1
#define XR_STDLIB_VM_BIND_CLASS_ONCE 1
#define XR_STDLIB_VM_BIND_CLASS_MUTEX 1
#define XR_STDLIB_VM_BIND_CLASS_RW_LOCK 1
#include "../../src/stdlib/xstdlib_class_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_CLASS_RW_LOCK
#undef XR_STDLIB_VM_BIND_CLASS_MUTEX
#undef XR_STDLIB_VM_BIND_CLASS_ONCE
#undef XR_STDLIB_VM_BIND_CLASS_BARRIER
#undef XR_STDLIB_VM_BIND_CLASS_CONDVAR

void xr_sys_mutex_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_mutex_class_generated(isolate);
}

void xr_sys_rwlock_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_rw_lock_class_generated(isolate);
}

void xr_sys_condvar_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_condvar_class_generated(isolate);
}

void xr_sys_barrier_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_barrier_class_generated(isolate);
}

void xr_sys_once_register_class(XrVMRuntime *isolate) {
    xr_stdlib_vm_register_once_class_generated(isolate);
}

#define XR_STDLIB_VM_BIND_MODULE_SYS 1
#include "../../src/stdlib/xstdlib_vm_bindings_generated.inc.c"
#undef XR_STDLIB_VM_BIND_MODULE_SYS

XrModule *xr_load_module_sys(XrVMRuntime *isolate) {
    XR_DCHECK(isolate != NULL, "xr_load_module_sys: NULL isolate");
    XrModule *module = xr_module_create_native(isolate, "sys");
    if (!module)
        return NULL;

    xr_stdlib_vm_bind_sys_generated(isolate, module);
    module->loaded = true;
    return module;
}
