/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xvm_helpers.c - VM helper functions
 *
 * KEY CONCEPT:
 *   Upvalue, closure, error handling and VM initialization helpers.
 */

#include "xvm_internal.h"
#include "../base/xchecks.h"
#include "../coro/xworker.h"
#include "../runtime/gc/xgc.h"
#include "../runtime/gc/xcoro_gc.h"
#include "../api/xglobal_object.h"
#include "../runtime/xerror_codes.h"
#include "../base/xsource_cache.h"

/* ========== Runtime Error Handling ========== */

/*
 * Report runtime error (diagnostic print only)
 *
 * Prints error message and call stack to stderr.
 * Does NOT modify VM state (no flag setting, no stack reset).
 * For catchable errors, use VM_RUNTIME_ERROR macro instead.
 */
void xr_runtime_error(XrayIsolate *isolate, const char *format, ...) {
    // Get execution context: prefer current coroutine, otherwise use main coroutine
    XrWorker *worker = xr_current_worker();
    XrVMContext *ctx = NULL;
    if (worker && worker->m->current_coro) {
        ctx = &((XrCoroutine *)worker->m->current_coro)->vm_ctx;
    } else if (isolate && isolate->main_coro) {
        ctx = &((XrCoroutine *)isolate->main_coro)->vm_ctx;
    }

    // Print error message
    fprintf(stderr, "\033[1;31merror\033[0m: ");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);

    // Get frame info
    int frame_count = ctx ? ctx->frame_count : 0;
    XrBcCallFrame *frames = ctx ? ctx->frames : NULL;

    // Show source code context (only top frame)
    if (frame_count > 0 && isolate->source_cache) {
        XrBcCallFrame *top_frame = &frames[frame_count - 1];
        if (top_frame->closure && top_frame->closure->proto) {
            XrProto *proto = top_frame->closure->proto;
            size_t instruction = top_frame->pc - PROTO_CODE_BASE(proto) - 1;
            size_t line_count = PROTO_LINE_COUNT(proto);
            int line = 0;
            if (line_count > 0) {
                size_t idx = (instruction < line_count) ? instruction : line_count - 1;
                line = PROTO_LINE(proto, idx);
            }

            if (line > 0 && proto->source_file) {
                // Try to get source code line
                const char *src_line = xr_source_cache_get_line(isolate->source_cache, proto->source_file, line);
                if (src_line) {
                    int line_len = xr_source_cache_get_line_length(isolate->source_cache, proto->source_file, line);
                    fprintf(stderr, "   |\n");
                    fprintf(stderr, " \033[1;34m%d\033[0m | %.*s\n", line, line_len, src_line);
                    fprintf(stderr, "   |\n");
                }
            }
        }
    }

    // Print call stack
    fprintf(stderr, "\033[1;36mstack trace:\033[0m\n");
    for (int i = frame_count - 1; i >= 0; i--) {
        XrBcCallFrame *frame = &frames[i];
        if (!frame->closure || !frame->closure->proto) continue;
        XrProto *proto = frame->closure->proto;

        // Calculate instruction offset
        size_t instruction = frame->pc - PROTO_CODE_BASE(proto) - 1;

        // Print filename
        if (proto->source_file != NULL) {
            fprintf(stderr, "  at %s:", proto->source_file);
        } else {
            fprintf(stderr, "  at ");
        }

        // Print line number
        size_t line_count = PROTO_LINE_COUNT(proto);
        int line = 0;
        if (line_count > 0) {
            size_t idx = (instruction < line_count) ? instruction : line_count - 1;
            line = PROTO_LINE(proto, idx);
            while (line == 0 && idx > 0) {
                idx--;
                line = PROTO_LINE(proto, idx);
            }
        }
        if (line > 0) {
            fprintf(stderr, "%d", line);
        } else {
            fprintf(stderr, "?");
        }

        // Print function name
        if (proto->name != NULL) {
            fprintf(stderr, " in %s()\n", proto->name->data);
        } else {
            fprintf(stderr, " in <main>\n");
        }
    }

}

// ========== Debug Info Query ==========

/*
 * Find local variable name by register number and PC
 * @param proto Function prototype
 * @param reg Register number
 * @param pc Current instruction index
 * @return Variable name, NULL if not found
 */
const char* xr_vm_get_local_name(XrProto *proto, int reg, int pc) {
    if (!proto) return NULL;

    int count = (int)PROTO_LOCVAR_COUNT(proto);
    // Search backwards, prefer most recently defined variable (handle same-name variable shadowing)
    for (int i = count - 1; i >= 0; i--) {
        XrLocVar lv = PROTO_LOCVAR(proto, i);
        // Check register match and within scope
        if (lv.reg == reg && pc >= lv.start_pc &&
            (lv.end_pc == -1 || pc <= lv.end_pc)) {
            return lv.name;
        }
    }
    return NULL;
}

// Cell / Context types and xr_cell_new live in runtime/closure/xcell.{h,c}.

// ========== C Function Operations ==========

/*
 * Create regular C function object
 */
XrCFunction *xr_vm_cfunction_new(XrayIsolate *isolate, XrCFunctionPtr func, const char *name) {
    XR_DCHECK(func != NULL, "cfunction_new: NULL func");
    XrCFunction *cfunc = (XrCFunction *)xr_malloc(sizeof(XrCFunction));
    if (cfunc == NULL) {
        return NULL;
    }

    // Initialize GC header, must set correct type XR_TCFUNCTION
    xr_gc_header_init_type(&cfunc->gc, XR_TCFUNCTION);
    cfunc->as.func = func;
    cfunc->name = name;
    cfunc->is_yieldable = false;
    atomic_init(&cfunc->cfunc_class, XR_CFUNC_FAST);
    atomic_init(&cfunc->auto_slow_count, 0);

    return cfunc;
}

/*
 * Create yieldable C function object
 */
XrCFunction *xr_vm_yieldable_cfunction_new(XrayIsolate *isolate, XrYieldableCFunctionPtr func, const char *name) {
    XR_DCHECK(func != NULL, "yieldable_cfunction_new: NULL func");
    XrCFunction *cfunc = (XrCFunction *)xr_malloc(sizeof(XrCFunction));
    if (cfunc == NULL) {
        return NULL;
    }

    // Initialize GC header, must set correct type XR_TCFUNCTION
    xr_gc_header_init_type(&cfunc->gc, XR_TCFUNCTION);
    cfunc->as.yieldable = func;
    cfunc->name = name;
    cfunc->is_yieldable = true;
    atomic_init(&cfunc->cfunc_class, XR_CFUNC_FAST);
    atomic_init(&cfunc->auto_slow_count, 0);

    return cfunc;
}

/*
 * Free C function object
 */
void xr_vm_cfunction_free(XrCFunction *cfunc) {
    if (cfunc != NULL) {
        xr_free(cfunc);
    }
}

// Closure creation now lives in runtime/closure/xclosure.c.

// ========== VM Initialization and Cleanup ==========

/*
 * Initialize virtual machine - accepts XrayIsolate parameter
 */
void xr_vm_vm_init(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "vm_init: NULL isolate");
    // Isolate already passed in, directly initialize VM state

    isolate->vm.stack_top = isolate->vm.stack;
    isolate->vm.frame_count = 0;

    // Initialize exception handling stack
    isolate->vm.handler_count = 0;
    isolate->vm.current_exception = xr_null();

    // Symbol table already initialized when XrayIsolate created (per-isolate)
    if (isolate && isolate->symbol_table) {
        XrSymbolTable *symtab = (XrSymbolTable*)isolate->symbol_table;
        (void)symtab;  // Avoid warning in non-DEBUG mode
        VM_DEBUG_PRINT("Using isolate symbol table with %d builtin symbols\n",
                      symtab->builtin_count);
    }

    // Initialize global variable array
    isolate->vm.builtin_count = 0;
    for (int i = 0; i < XR_GLOBALS_MAX; i++) {
        isolate->vm.builtins[i] = xr_null();
    }

    // Initialize dynamic shared variable array
    xr_shared_array_init(&isolate->vm.shared);

    // Set reflection API class as global variable
    if (isolate && isolate->core) {
        // Register Reflect class to global variable index 0
        if (isolate->core->reflectClass) {
            isolate->vm.builtins[0] = xr_value_from_class(isolate->core->reflectClass);
            if (isolate->vm.builtin_count < 1) isolate->vm.builtin_count = 1;
            VM_DEBUG_PRINT("Reflect class registered as global variable (index=0)\n");
        }
    }

    // Global constructor registration (register as class objects to support static methods)
    if (isolate && isolate->core) {
        // Array class
        if (isolate->core->arrayClass) {
            isolate->vm.builtins[XR_GLOBAL_VAR_ARRAY] = xr_value_from_class(isolate->core->arrayClass);
        }

        // Set class
        if (isolate->core->setClass) {
            isolate->vm.builtins[XR_GLOBAL_VAR_SET] = xr_value_from_class(isolate->core->setClass);
        }

        // Map class
        if (isolate->core->mapClass) {
            isolate->vm.builtins[XR_GLOBAL_VAR_MAP] = xr_value_from_class(isolate->core->mapClass);
        }

        // String class
        if (isolate->core->stringClass) {
            isolate->vm.builtins[XR_GLOBAL_VAR_STRING] = xr_value_from_class(isolate->core->stringClass);
        }

        // Json utility class
        if (isolate->core->jsonClass) {
            isolate->vm.builtins[XR_GLOBAL_VAR_JSON] = xr_value_from_class(isolate->core->jsonClass);
        }

        // process/__file__/__dir__ indices 5/6/7, user global variables start from XR_USER_GLOBALS_START
        if (isolate->vm.builtin_count < XR_USER_GLOBALS_START) isolate->vm.builtin_count = XR_USER_GLOBALS_START;
        VM_DEBUG_PRINT("Global constructors registered: Array, Set, Map, String\n");
    }

    // Built-in methods should be added via ClassBuilder during class initialization
    // TODO: Migrate all built-in methods to xclass_system.c

    // Global variables use array, no hash table needed

    // Initialize string interning table
    isolate->vm.strings_map = xr_hashmap_new();

    // GC initialization
    // isolate->vm.objects removed - objects automatically managed by GC system
    isolate->vm.bytes_allocated = 0;
    isolate->vm.next_gc = 1024 * 1024;  // 1MB

    // Debug options
    isolate->vm.trace_execution = false;

}

/*
 * Free virtual machine
 */
void xr_vm_vm_free(XrayIsolate *isolate) {
    XR_DCHECK(isolate != NULL, "vm_free: NULL isolate");
    // Global variables use array, no hash table to free

    // Free string interning table
    if (isolate->vm.strings_map != NULL) {
        xr_hashmap_free(isolate->vm.strings_map);
        isolate->vm.strings_map = NULL;
    }

    // Free all GC objects
    // NOTE: XrObject has been removed, object freeing handled automatically by GC system
    // isolate->vm.objects removed - objects automatically managed by GC system
}

// ========== Value Operation Helpers ==========

/*
 * Check if value is truthy (public API, for higher-order functions)
 * Uses vm_is_falsey from xvm_internal.h for consistent behavior
 */
bool xr_vm_is_truthy(XrValue value) {
    return !vm_is_falsey(value);
}

// ========== VM Execution Loop ==========
