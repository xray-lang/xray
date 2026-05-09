/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass.c - Class object implementation
 *
 * KEY CONCEPT:
 *   Static class structure with O(1) field/method lookup.
 *   Supports inheritance, interfaces, and operator overloading.
 */

#include "xclass.h"
#include "xclass_internal.h"
#include "xclass_builder.h"
#include "xinstance.h"
#include "xmethod.h"
#include "xenum.h"
#include "xclass_lookup.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../xisolate_api.h"
#include "../object/xnative_type.h"
#include "xclass_system.h"
#include "xreflect_registry.h"
#include "xreflect_cache.h"
#include "../value/xvalue.h"
#include "../symbol/xsymbol_table.h"
#include <stdio.h>
#include <string.h>

/* ========== Operator Flag Mapping ========== */

// Map an operator method symbol to its class-level operator_flags bit.
// Written as a dense switch so the compiler emits a jump table instead
// of the 22 sequential compares the old chain produced.
uint32_t xr_symbol_to_op_flag(int symbol) {
    if (symbol <= 0)
        return 0;

    switch (symbol) {
        case SYMBOL_OP_ADD:
            return XR_OP_ADD_FLAG;
        case SYMBOL_OP_SUB:
            return XR_OP_SUB_FLAG;
        case SYMBOL_OP_MUL:
            return XR_OP_MUL_FLAG;
        case SYMBOL_OP_DIV:
            return XR_OP_DIV_FLAG;
        case SYMBOL_OP_MOD:
            return XR_OP_MOD_FLAG;
        case SYMBOL_OP_EQ:
            return XR_OP_EQ_FLAG;
        case SYMBOL_OP_NE:
            return XR_OP_NE_FLAG;
        case SYMBOL_OP_LT:
            return XR_OP_LT_FLAG;
        case SYMBOL_OP_LE:
            return XR_OP_LE_FLAG;
        case SYMBOL_OP_GT:
            return XR_OP_GT_FLAG;
        case SYMBOL_OP_GE:
            return XR_OP_GE_FLAG;
        case SYMBOL_OP_BAND:
            return XR_OP_BAND_FLAG;
        case SYMBOL_OP_BOR:
            return XR_OP_BOR_FLAG;
        case SYMBOL_OP_BXOR:
            return XR_OP_BXOR_FLAG;
        case SYMBOL_OP_BNOT:
            return XR_OP_BNOT_FLAG;
        case SYMBOL_OP_LSHIFT:
            return XR_OP_LSHIFT_FLAG;
        case SYMBOL_OP_RSHIFT:
            return XR_OP_RSHIFT_FLAG;
        case SYMBOL_OP_INDEX:
            return XR_OP_INDEX_FLAG;
        case SYMBOL_OP_INDEX_SET:
            return XR_OP_INDEX_SET_FLAG;
        case SYMBOL_OP_INC:
            return XR_OP_INC_FLAG;
        case SYMBOL_OP_DEC:
            return XR_OP_DEC_FLAG;
        case SYMBOL_OP_NOT:
            return XR_OP_NOT_FLAG;
        default:
            return 0;
    }
}

// Compute operator overload flags (call once after class creation)
void xr_class_compute_operator_flags(XrClass *cls) {
    if (!cls)
        return;

    cls->operator_flags = 0;

    // Inherit parent's operator flags
    if (cls->super) {
        cls->operator_flags = cls->super->operator_flags;
    }
    if (cls->methods && cls->method_count > 0) {
        for (int i = 0; i < cls->method_count; i++) {
            XrMethod *method = &cls->methods[i];

            if (method->type == XMETHOD_NONE) {
                continue;
            }
            if (method->type == XMETHOD_OPERATOR) {
                uint32_t flag = xr_symbol_to_op_flag(method->symbol);
                cls->operator_flags |= flag;
            }
        }
    }
}

/* ========== Class Object Implementation ========== */

XrClass *xr_class_new(XrayIsolate *X, const char *name, XrClass *super) {
    XR_DCHECK(name != NULL, "Class name must not be NULL");

    // xr_class_new used to allocate a raw XrClass by hand and then, when a
    // super was given, patch the inheritance in place via xr_class_set_super
    // -- a ~280-line operation that re-flattened fields / methods / vtable
    // after the class had already been assembled with the wrong parent.
    //
    // The descriptor path already produces the correct class in one shot
    // through the builder. Bare classes used by core init (Object, String,
    // Int ...) and by enum creation are structurally equivalent to the
    // output of "empty builder + finalize", so we funnel everything
    // through the same code path.
    //
    // Callers remain responsible for reflection registration; xr_core_init
    // batches it for the builtin classes and xr_enum_type_new registers
    // each enum class right after construction.
    XrClassBuilder *builder = xr_class_builder_new(X, name, super);
    if (!builder) {
        xr_log_warning("class", "class_new: builder allocation failed for '%s'", name);
        return NULL;
    }
    return xr_class_builder_finalize(builder);
}

// xr_class_set_super has been removed. All inheritance is established in
// one shot at build time via xr_class_builder_finalize; there is no
// longer a supported API for patching a class's super link after the
// class has been frozen.

const XrFieldDescriptor *xr_class_get_field(const XrClass *cls, int index) {
    if (!cls || index < 0 || index >= cls->field_count) {
        return NULL;
    }
    XR_DCHECK(cls->fields != NULL, "class_get_field: NULL fields array");
    return &cls->fields[index];
}

int xr_class_lookup_field_by_name(XrayIsolate *X, XrClass *cls, const char *name) {
    if (!X || !cls || !name)
        return -1;

    // Resolve the name through the symbol table (O(1) hash) rather
    // than scanning every field descriptor with strcmp. The symbol
    // table owns every interned name and xr_class_lookup_field has
    // its own O(1) symbol -> index mapping, so the full path is a
    // pair of hash hits without any string compares.
    XrSymbolTable *tab = (XrSymbolTable *) xr_isolate_get_symbol_table(X);
    if (!tab)
        return -1;
    SymbolId sym = xr_symbol_lookup_in_table(tab, name);
    if (sym == SYMBOL_INVALID)
        return -1;

    return xr_class_lookup_field(cls, (int) sym);
}

// O(1) lookup + O(depth) recursive parent lookup
int xr_class_lookup_field(XrClass *cls, int symbol) {
    if (!cls || symbol < 0)
        return -1;

    // O(1) symbol mapping lookup
    if (cls->field_symbol_to_index && cls->field_map_capacity > 0) {
        if (symbol >= 0 && symbol < cls->field_map_capacity) {
            int field_idx = cls->field_symbol_to_index[symbol];
            if (field_idx >= 0) {
                return field_idx;
            }
        }
    }

    // Recursive parent lookup
    if (cls->super) {
        return xr_class_lookup_field(cls->super, symbol);
    }

    return -1;
}

// Method lookup by symbol.
// Precondition: every class built through the builder has
// method_symbol_to_index populated for any symbol it declares. Classes
// created via xr_class_new() that have no methods leave the mapping NULL;
// in that case we fall through to super. There is no O(n) sweep: if a
// symbol is absent from the mapping, it is absent from this class.
XrMethod *xr_class_lookup_method(XrClass *cls, int symbol) {
    if (!cls || symbol < 0)
        return NULL;

    if (cls->method_symbol_to_index && symbol < cls->method_map_capacity) {
        int method_idx = cls->method_symbol_to_index[symbol];

        if (method_idx >= 0 && method_idx < cls->method_count) {
            XrMethod *method = &cls->methods[method_idx];

            if (method->symbol == symbol && method->type != XMETHOD_NONE) {
                // VTable optimization for closure methods only;
                // primitive methods do not use vtable dispatch.
                if (method->type != XMETHOD_PRIMITIVE && method->vtable_index >= 0 && cls->vtable &&
                    method->vtable_index < cls->vtable_size) {
                    XrMethod *vtable_method = cls->vtable[method->vtable_index];
                    if (vtable_method && vtable_method->type != XMETHOD_NONE) {
                        return vtable_method;
                    }
                }
                return method;
            }
        }
    }

    if (cls->super) {
        return xr_class_lookup_method(cls->super, symbol);
    }

    return NULL;
}

/* ========== Value Type Access ========== */

// Get class for any value (unified object model)
XrClass *xr_value_get_class(XrayIsolate *X, XrValue value) {
    XR_DCHECK(X != NULL, "value_get_class: NULL isolate");

    /* Resolve the XrObjType. Value types (int/float/bool/null) map to
     * the enum directly; heap objects read the GC header type tag. */
    XrObjType type;
    if (XR_IS_INT(value))
        type = XR_TINT;
    else if (XR_IS_FLOAT(value))
        type = XR_TFLOAT;
    else if (XR_IS_BOOL(value))
        type = XR_TBOOL;
    else if (XR_IS_NULL(value))
        type = XR_TNULL;
    else if (XR_IS_PTR(value))
        type = XR_GC_GET_TYPE((XrGCHeader *) XR_TO_PTR(value));
    else
        return NULL;

    /* Instance: class pointer stored in the object header. */
    if (type == XR_TINSTANCE) {
        XrInstance *inst = (XrInstance *) XR_TO_PTR(value);
        return inst->klass;
    }

    /* Enum value: resolve by name, fall back to the abstract Enum class. */
    if (type == XR_TENUM_VALUE) {
        XrEnumValue *ev = (XrEnumValue *) XR_TO_PTR(value);
        if (ev->enum_name) {
            XrClass *cls = xr_class_lookup_by_name(X, ev->enum_name);
            if (cls)
                return cls;
        }
        XrayCoreClasses *core = xr_isolate_get_core_classes(X);
        return core ? core->enumClass : NULL;
    }

    /* Enum type: each enum type carries its own class. */
    if (type == XR_TENUM_TYPE) {
        XrEnumType *et = (XrEnumType *) XR_TO_PTR(value);
        return et->enum_class;
    }

    /* All other types: single lookup in native_type_classes[].
     * This covers primitives (int/float/bool/null), collections
     * (Array/Map/Set/String/Json), stdlib types (DateTime/Regex/...),
     * and internal types (Iterator/Range/StringBuilder/BigInt). */
    if ((int) type < XR_NATIVE_TYPE_MAX)
        return xr_isolate_get_native_type_class(X, type);

    return NULL;
}

/* ========== Helper Functions ========== */

// Debug: print class info
void xr_class_print(XrClass *cls) {
    if (!cls) {
        printf("null class\n");
        return;
    }

    printf("Class %s", cls->name);
    if (cls->super) {
        printf(" extends %s", cls->super->name);
    }
    printf(" {\n");

    printf("  Fields (%d):\n", cls->field_count);
    for (int i = 0; i < cls->field_count; i++) {
        if (cls->fields && cls->fields[i].name) {
            printf("    [%d] %s", i, cls->fields[i].name);
            if (cls->fields[i].flags & XR_FIELD_PRIVATE) {
                printf(" (private)");
            }
            printf("\n");
        }
    }

    int method_count = 0;
    for (int i = 0; i < cls->method_count; i++) {
        if (cls->methods[i].type != XMETHOD_NONE)
            method_count++;
    }
    printf("  Methods: %d (array size: %d)\n", method_count, cls->method_count);
    printf("  Static methods: %d\n", cls->static_method_count);
    printf("}\n");
}

// Static and instance methods stored together
// Use xr_class_lookup_method() for all methods
// Check method->flags & XMETHOD_FLAG_STATIC for static methods

/* ========== Interface Support ========== */

XrClass *xr_interface_new(XrayIsolate *X, const char *name) {
    XR_DCHECK(X != NULL, "interface_new: NULL isolate");
    XR_DCHECK(name != NULL, "Interface name must not be NULL");

    // Interfaces are just bare classes flagged as XR_CLASS_INTERFACE.
    // Build them through the same path as any other class so there is a
    // single code path for the (name + super) + finalize pipeline.
    XrClassBuilder *builder = xr_class_builder_new(X, name, NULL);
    if (!builder)
        return NULL;
    XrClass *iface = xr_class_builder_finalize(builder);
    if (iface)
        iface->flags |= XR_CLASS_INTERFACE;
    return iface;
}

bool xr_class_implements_interface(XrClass *cls, XrClass *iface) {
    if (!cls || !iface || !(iface->flags & XR_CLASS_INTERFACE))
        return false;

    // Walk the inheritance chain iteratively; an interface declared on
    // any ancestor counts as implemented. Pointer identity compare
    // throughout -- no strcmp, no name-based resolution.
    for (XrClass *cur = cls; cur != NULL; cur = cur->super) {
        for (int i = 0; i < cur->interface_count; i++) {
            if (cur->interfaces[i] == iface) {
                return true;
            }
        }
    }
    return false;
}

/* ========== ITable Generation ========== */

int xr_class_build_itable(XrClass *cls) {
    if (!cls)
        return -1;

    // Free existing itable (including per-entry methods[] and per-entry
    // method_symbol_to_index maps that a previous build may have left).
    if (cls->itable) {
        for (int i = 0; i < cls->itable_size; i++) {
            if (cls->itable[i].methods) {
                xr_free(cls->itable[i].methods);
            }
            if (cls->itable[i].method_symbol_to_index) {
                xr_free(cls->itable[i].method_symbol_to_index);
            }
        }
        xr_free(cls->itable);
        cls->itable = NULL;
        cls->itable_size = 0;
    }

    if (!cls->interfaces || cls->interface_count == 0) {
        return 0;
    }

    cls->itable_size = cls->interface_count;
    cls->itable = (XrItableEntry *) xr_malloc(cls->itable_size * sizeof(XrItableEntry));
    if (!cls->itable) {
        cls->itable_size = 0;
        return -1;
    }
    memset(cls->itable, 0, cls->itable_size * sizeof(XrItableEntry));

    int result = 0;

    for (int i = 0; i < cls->interface_count; i++) {
        XrClass *iface = cls->interfaces[i];
        if (!iface)
            continue;

        cls->itable[i].interface = iface;
        cls->itable[i].method_count = iface->method_count;

        if (iface->method_count > 0) {
            cls->itable[i].methods =
                (XrMethod **) xr_malloc(iface->method_count * sizeof(XrMethod *));
            if (!cls->itable[i].methods) {
                // methods[] alloc failed -> unwind every itable entry built
                // so far. Centralised in the `fail` label so we never mix
                // a "half-built" itable into cls on the error path.
                result = -1;
                goto fail;
            }

            // Resolve each interface method via the class's
            // symbol-to-index table. method_symbol_to_index is populated
            // by the builder for every class with at least one method,
            // so this is O(1) per slot; the previous inner loop was
            // O(cls->method_count) per interface method.
            int max_symbol = -1;
            for (int j = 0; j < iface->method_count; j++) {
                XrMethod *iface_method = &iface->methods[j];
                XrMethod *impl = NULL;

                int sym = iface_method->symbol;
                if (cls->method_symbol_to_index && sym >= 0 && sym < cls->method_map_capacity) {
                    int idx = cls->method_symbol_to_index[sym];
                    if (idx >= 0 && idx < cls->method_count) {
                        impl = &cls->methods[idx];
                    }
                }

                cls->itable[i].methods[j] = impl;
                if (sym > max_symbol)
                    max_symbol = sym;
            }

            // Build the per-entry symbol -> slot map so the runtime
            // dispatch path xr_class_lookup_interface_method_by_symbol
            // can jump directly to a slot instead of walking the
            // methods[] array. A map allocation failure is tolerated
            // here -- lookup_by_symbol handles the NULL-map case with
            // a direct "entry has no map => return NULL" fallback.
            if (max_symbol >= 0) {
                int cap = max_symbol + 1;
                int *map = (int *) xr_malloc(cap * sizeof(int));
                if (map) {
                    for (int k = 0; k < cap; k++)
                        map[k] = -1;
                    for (int j = 0; j < iface->method_count; j++) {
                        int sym = iface->methods[j].symbol;
                        if (sym >= 0 && sym < cap)
                            map[sym] = j;
                    }
                    cls->itable[i].method_symbol_to_index = map;
                    cls->itable[i].method_map_capacity = cap;
                }
            }
        } else {
            cls->itable[i].methods = NULL;
        }
    }

    return 0;

fail:
    // Release every partial entry and the itable array itself so the
    // class is left with (itable == NULL, itable_size == 0) just like
    // the pre-build state -- xr_class_free and a future retry of
    // build_itable both see a clean slate.
    for (int i = 0; i < cls->itable_size; i++) {
        if (cls->itable[i].methods) {
            xr_free(cls->itable[i].methods);
            cls->itable[i].methods = NULL;
        }
        if (cls->itable[i].method_symbol_to_index) {
            xr_free(cls->itable[i].method_symbol_to_index);
            cls->itable[i].method_symbol_to_index = NULL;
        }
    }
    xr_free(cls->itable);
    cls->itable = NULL;
    cls->itable_size = 0;
    return result;
}

/* ========== Abstract Class Support ========== */

void xr_class_mark_abstract(XrClass *cls) {
    if (!cls)
        return;
    cls->flags |= XR_CLASS_ABSTRACT;
}

void xr_class_add_abstract_method(XrClass *cls, int method_symbol) {
    if (!cls || method_symbol < 0)
        return;

    int new_count = cls->abstract_method_count + 1;
    int *new_methods = (int *) xr_realloc(cls->abstract_methods, sizeof(int) * new_count);
    if (!new_methods)
        return;
    cls->abstract_methods = new_methods;
    cls->abstract_method_count = new_count;
    cls->abstract_methods[new_count - 1] = method_symbol;
}

// Check if class can be instantiated (not abstract and all abstract methods implemented)
bool xr_class_can_instantiate(XrClass *cls) {
    if (!cls)
        return false;

    if (cls->flags & XR_CLASS_ABSTRACT)
        return false;

    for (int i = 0; i < cls->abstract_method_count; i++) {
        int symbol = cls->abstract_methods[i];
        XrMethod *method = xr_class_lookup_method(cls, symbol);

        if (!method || method->type == XMETHOD_NONE || xr_method_is_abstract(method)) {
            return false;
        }
    }

    return true;
}

// Inherit abstract methods from parent
void xr_class_inherit_abstract_methods(XrClass *child, XrClass *parent) {
    if (!child || !parent)
        return;

    if (parent->abstract_method_count == 0)
        return;

    for (int i = 0; i < parent->abstract_method_count; i++) {
        int symbol = parent->abstract_methods[i];
        XrMethod *child_method = xr_class_lookup_method(child, symbol);

        if (!child_method || child_method->type == XMETHOD_NONE ||
            xr_method_is_abstract(child_method)) {
            xr_class_add_abstract_method(child, symbol);
        }
    }
}

bool xr_class_is_abstract_method(XrClass *cls, int method_symbol) {
    if (!cls || method_symbol < 0)
        return false;

    for (int i = 0; i < cls->abstract_method_count; i++) {
        if (cls->abstract_methods[i] == method_symbol) {
            return true;
        }
    }

    return false;
}

// instanceof operator implementation (O(1) with primary_supers)
bool xr_instance_of(void *obj, const XrClass *target) {
    if (obj == NULL || target == NULL) {
        return false;
    }

    XrGCHeader *gc = (XrGCHeader *) obj;
    XrObjType type = XR_GC_GET_TYPE(gc);

    if (type != XR_TINSTANCE) {
        return false;
    }

    XrInstance *inst = (XrInstance *) obj;
    XrClass *obj_class = inst->klass;

    if (obj_class == NULL) {
        return false;
    }

    return xr_class_instanceof(obj_class, target);
}

// Interface method lookup (by index / by symbol) used to live here
// but had no callers anywhere in the tree -- every dispatch site
// goes through xr_class_lookup_method on the declaring class. The
// pair was removed together with xr_class_has_method when the last
// of the "scan classes by name or search interfaces by name" code
// paths went away.

/*
 * Free class resources
 * Note: Classes are typically GC-managed; this is for explicit cleanup during error paths
 */
void xr_class_free(XrClass *cls) {
    if (!cls)
        return;

    // Free method table
    if (cls->methods) {
        xr_free(cls->methods);
        cls->methods = NULL;
    }

    // Free method symbol lookup table
    if (cls->method_symbol_to_index) {
        xr_free(cls->method_symbol_to_index);
        cls->method_symbol_to_index = NULL;
    }

    // Free instance fields table
    if (cls->fields) {
        xr_free(cls->fields);
        cls->fields = NULL;
    }

    // Free field symbol lookup table
    if (cls->field_symbol_to_index) {
        xr_free(cls->field_symbol_to_index);
        cls->field_symbol_to_index = NULL;
    }

    // Free field default values
    if (cls->field_default_values) {
        xr_free(cls->field_default_values);
        cls->field_default_values = NULL;
    }

    // Free vtable
    if (cls->vtable) {
        xr_free(cls->vtable);
        cls->vtable = NULL;
    }

    // Free static field values
    if (cls->static_field_values) {
        xr_free(cls->static_field_values);
        cls->static_field_values = NULL;
    }

    // Free interface list
    if (cls->interfaces) {
        xr_free(cls->interfaces);
        cls->interfaces = NULL;
    }

    // Free itable entries (methods[] and per-entry symbol map).
    if (cls->itable) {
        for (int i = 0; i < cls->itable_size; i++) {
            if (cls->itable[i].methods) {
                xr_free(cls->itable[i].methods);
            }
            if (cls->itable[i].method_symbol_to_index) {
                xr_free(cls->itable[i].method_symbol_to_index);
            }
        }
        xr_free(cls->itable);
        cls->itable = NULL;
    }

    // Free abstract method indices
    if (cls->abstract_methods) {
        xr_free(cls->abstract_methods);
        cls->abstract_methods = NULL;
    }

    // Free reflection cache
    if (cls->reflect_cache) {
        xr_reflect_cache_free(cls->reflect_cache);
        cls->reflect_cache = NULL;
    }

    // Free secondary supers hash (only allocated when depth >= 8).
    if (cls->secondary_supers_hash) {
        xr_free(cls->secondary_supers_hash);
        cls->secondary_supers_hash = NULL;
        cls->secondary_supers_capacity = 0;
    }

    // Free monomorphized type arg names array
    if (cls->mono_type_arg_names) {
        for (uint8_t i = 0; i < cls->mono_type_argc; i++)
            xr_free((void *) cls->mono_type_arg_names[i]);
        xr_free(cls->mono_type_arg_names);
        cls->mono_type_arg_names = NULL;
        cls->mono_type_argc = 0;
    }

    // Class name is interned string, not freed here

    xr_free(cls);
}
