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
#include "../runtime/gc/xheap.h"
#include "../runtime/gc/xcoro_heap.h"
#include "../runtime/xerror_codes.h"
#include "../runtime/value/xstruct_layout.h"
#include "../base/xsource_cache.h"

/* ========== Struct Layout Registry ========== */

static void xr_vm_struct_layout_register_children(XrVMState *vm, XrStructLayout *layout) {
    if (!vm || !layout)
        return;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        XrStructFieldLayout *field = &layout->fields[i];
        if (field->native_type == XR_NATIVE_STRUCT && field->sub_layout) {
            field->sub_layout_id = xr_vm_struct_layout_register(vm, field->sub_layout);
        }
    }
}

uint16_t xr_vm_struct_layout_register(XrVMState *vm, XrStructLayout *layout) {
    if (!vm || !layout)
        return 0;

    xr_vm_struct_layout_register_children(vm, layout);

    if (layout->layout_id != 0) {
        XrStructLayout *registered = xr_vm_struct_layout_lookup(vm, layout->layout_id);
        if (registered == layout)
            return layout->layout_id;
    }

    for (uint16_t i = 1; i < vm->struct_layout_count; i++) {
        if (vm->struct_layouts && vm->struct_layouts[i] == layout) {
            layout->layout_id = i;
            return i;
        }
    }

    if (vm->struct_layout_count == UINT16_MAX)
        return 0;

    if (vm->struct_layout_capacity == 0) {
        uint16_t cap = 16;
        vm->struct_layouts = (XrStructLayout **) xr_calloc(cap, sizeof(*vm->struct_layouts));
        if (!vm->struct_layouts)
            return 0;
        vm->struct_layout_capacity = cap;
        vm->struct_layout_count = 1;
    } else if (vm->struct_layout_count >= vm->struct_layout_capacity) {
        uint16_t old_cap = vm->struct_layout_capacity;
        uint16_t new_cap = (old_cap <= UINT16_MAX / 2) ? (uint16_t) (old_cap * 2) : UINT16_MAX;
        XrStructLayout **new_layouts = (XrStructLayout **) xr_realloc(
            vm->struct_layouts, (size_t) new_cap * sizeof(*new_layouts));
        if (!new_layouts)
            return 0;
        memset(new_layouts + old_cap, 0, (size_t) (new_cap - old_cap) * sizeof(*new_layouts));
        vm->struct_layouts = new_layouts;
        vm->struct_layout_capacity = new_cap;
    }

    uint16_t id = vm->struct_layout_count++;
    vm->struct_layouts[id] = layout;
    layout->layout_id = id;
    return id;
}

XrStructLayout *xr_vm_struct_layout_lookup(XrVMState *vm, uint16_t layout_id) {
    if (!vm || layout_id == 0 || layout_id >= vm->struct_layout_count || !vm->struct_layouts)
        return NULL;
    return vm->struct_layouts[layout_id];
}

XrStructLayout *xr_vm_struct_ref_layout(XrayIsolate *isolate, XrValue ref) {
    if (!XR_IS_STRUCT_REF(ref) || XR_IS_ARRAY_REF(ref) || !ref.ptr)
        return NULL;

    uint16_t layout_id = xr_struct_layout_id(ref);
    if (layout_id != 0 && isolate) {
        XrStructLayout *layout = xr_vm_struct_layout_lookup(&isolate->vm, layout_id);
        if (layout)
            return layout;
    }

    XrClass *cls = *(XrClass **) ref.ptr;
    if (!cls || !cls->struct_layout)
        return NULL;
    if (isolate)
        xr_vm_struct_layout_register(&isolate->vm, cls->struct_layout);
    return cls->struct_layout;
}

uint8_t *xr_vm_struct_ref_payload(XrayIsolate *isolate, XrValue ref, XrStructLayout **layout_out) {
    XrStructLayout *layout = xr_vm_struct_ref_layout(isolate, ref);
    if (layout_out)
        *layout_out = layout;
    if (!layout || !ref.ptr)
        return NULL;
    return (uint8_t *) ref.ptr + xr_struct_layout_header_size(layout);
}

int xr_vm_struct_layout_field_index(XrayIsolate *isolate, const XrStructLayout *layout,
                                    int prop_symbol) {
    if (!isolate || !layout || !layout->field_names)
        return -1;
    XrSymbolTable *sym_table = (XrSymbolTable *) isolate->core_rt->symbol_table;
    const char *prop_name = xr_symbol_get_name_in_table(sym_table, prop_symbol);
    if (!prop_name)
        return -1;
    for (uint16_t i = 0; i < layout->field_count; i++) {
        if (layout->field_names[i] && strcmp(layout->field_names[i], prop_name) == 0)
            return (int) i;
    }
    return -1;
}

/* ========== Runtime Error Handling ========== */

/*
 * Report runtime error (diagnostic print only)
 *
 * Prints error message and call stack to stderr.
 * Does NOT modify VM state (no flag setting, no stack reset).
 * For catchable errors, use VM_RUNTIME_ERROR macro instead.
 */
void xr_runtime_error(XrayIsolate *isolate, const char *format, ...) {
    // Single authoritative ctx resolver — no bespoke fallback chain.
    XrVMContext *ctx = isolate ? xr_vm_current_ctx(isolate) : NULL;

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
                const char *src_line =
                    xr_source_cache_get_line(isolate->source_cache, proto->source_file, line);
                if (src_line) {
                    int line_len = xr_source_cache_get_line_length(isolate->source_cache,
                                                                   proto->source_file, line);
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
        if (!frame->closure || !frame->closure->proto)
            continue;
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
const char *xr_vm_get_local_name(XrProto *proto, int reg, int pc) {
    if (!proto)
        return NULL;

    int count = (int) PROTO_LOCVAR_COUNT(proto);
    // Search backwards, prefer most recently defined variable (handle same-name variable shadowing)
    for (int i = count - 1; i >= 0; i--) {
        XrLocVar lv = PROTO_LOCVAR(proto, i);
        // Check register match and within scope
        if (lv.reg == reg && pc >= lv.start_pc && (lv.end_pc == -1 || pc <= lv.end_pc)) {
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
    XrCFunction *cfunc = (XrCFunction *) xr_malloc(sizeof(XrCFunction));
    if (cfunc == NULL) {
        return NULL;
    }

    // Fully initialize object header. xr_malloc returns uninitialized memory,
    // so the fields not set below must be cleared explicitly.
    memset(&cfunc->hdr, 0, sizeof(XrObjHeader));
    cfunc->hdr.type = XR_TCFUNCTION;
    cfunc->hdr.objsize = (uint32_t) sizeof(XrCFunction);
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
XrCFunction *xr_vm_yieldable_cfunction_new(XrayIsolate *isolate, XrYieldableCFunctionPtr func,
                                           const char *name) {
    XR_DCHECK(func != NULL, "yieldable_cfunction_new: NULL func");
    XrCFunction *cfunc = (XrCFunction *) xr_malloc(sizeof(XrCFunction));
    if (cfunc == NULL) {
        return NULL;
    }

    // Fully initialize object header — see xr_vm_cfunction_new for rationale.
    memset(&cfunc->hdr, 0, sizeof(XrObjHeader));
    cfunc->hdr.type = XR_TCFUNCTION;
    cfunc->hdr.objsize = (uint32_t) sizeof(XrCFunction);
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
    isolate->vm.pending_error = xr_null();

    // Symbol table already initialized when XrayIsolate created (per-isolate)
    if (isolate && isolate->core_rt->symbol_table) {
        XrSymbolTable *symtab = (XrSymbolTable *) isolate->core_rt->symbol_table;
        (void) symtab;  // Avoid warning in non-DEBUG mode
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
            if (isolate->vm.builtin_count < 1)
                isolate->vm.builtin_count = 1;
            VM_DEBUG_PRINT("Reflect class registered as global variable (index=0)\n");
        }
    }

    // Global constructor registration (register as class objects to support static methods)
    if (isolate && isolate->core) {
        // Array class
        if (isolate->core->arrayClass) {
            isolate->vm.builtins[XR_GLOBAL_VAR_ARRAY] =
                xr_value_from_class(isolate->core->arrayClass);
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
            isolate->vm.builtins[XR_GLOBAL_VAR_STRING] =
                xr_value_from_class(isolate->core->stringClass);
        }

        // Json utility class
        if (isolate->core->jsonClass) {
            isolate->vm.builtins[XR_GLOBAL_VAR_JSON] =
                xr_value_from_class(isolate->core->jsonClass);
        }

        if (isolate->core_rt->native_type_classes[XR_TWORKQUEUE]) {
            isolate->vm.builtins[XR_GLOBAL_VAR_WORKQUEUE] =
                xr_value_from_class(isolate->core_rt->native_type_classes[XR_TWORKQUEUE]);
        }
        if (isolate->core_rt->native_type_classes[XR_TRESULTGROUP]) {
            isolate->vm.builtins[XR_GLOBAL_VAR_RESULTGROUP] =
                xr_value_from_class(isolate->core_rt->native_type_classes[XR_TRESULTGROUP]);
        }

        // process/__file__/__dir__ indices 5/6/7, user global variables start from
        // XR_USER_GLOBALS_START
        if (isolate->vm.builtin_count < XR_USER_GLOBALS_START)
            isolate->vm.builtin_count = XR_USER_GLOBALS_START;
        VM_DEBUG_PRINT("Global constructors registered: Array, Set, Map, String\n");
    }

    // Built-in methods should be added via ClassBuilder during class initialization
    // TODO: Migrate all built-in methods to xclass_system.c

    // Global variables use array, no hash table needed

    // Initialize string interning table
    isolate->vm.strings_map = xr_hashmap_new();

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

    xr_free(isolate->vm.struct_layouts);
    isolate->vm.struct_layouts = NULL;
    isolate->vm.struct_layout_count = 0;
    isolate->vm.struct_layout_capacity = 0;
}

// ========== Value Operation Helpers ==========

/*
 * Check if value is truthy (public API, for higher-order functions)
 * Uses vm_is_falsey from xvm_internal.h for consistent behavior
 */
bool xr_vm_is_truthy(XrValue value) {
    return xr_value_is_truthy(value);
}

// ========== VM Execution Loop ==========
