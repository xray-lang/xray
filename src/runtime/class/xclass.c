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
#include "xinstance.h"
#include "xmethod.h"
#include "xenum.h"
#include "xclass_lookup.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include "../../base/xlog.h"
#include "../xisolate_api.h"
#include "xclass_system.h"
#include "xreflect_registry.h"
#include "xreflect_cache.h"
#include "../value/xvalue.h"
#include "../symbol/xsymbol_table.h"
#include <stdio.h>
#include <string.h>

/* ========== Operator Flag Mapping ========== */

// Map operator symbol to flag (linear search, 22 operators max)
uint32_t xr_symbol_to_op_flag(int symbol) {
    if (symbol <= 0) {
        return 0;
    }
    if (symbol == SYMBOL_OP_ADD) return XR_OP_ADD_FLAG;
    if (symbol == SYMBOL_OP_SUB) return XR_OP_SUB_FLAG;
    if (symbol == SYMBOL_OP_MUL) return XR_OP_MUL_FLAG;
    if (symbol == SYMBOL_OP_DIV) return XR_OP_DIV_FLAG;
    if (symbol == SYMBOL_OP_MOD) return XR_OP_MOD_FLAG;
    if (symbol == SYMBOL_OP_EQ) return XR_OP_EQ_FLAG;
    if (symbol == SYMBOL_OP_NE) return XR_OP_NE_FLAG;
    if (symbol == SYMBOL_OP_LT) return XR_OP_LT_FLAG;
    if (symbol == SYMBOL_OP_LE) return XR_OP_LE_FLAG;
    if (symbol == SYMBOL_OP_GT) return XR_OP_GT_FLAG;
    if (symbol == SYMBOL_OP_GE) return XR_OP_GE_FLAG;
    if (symbol == SYMBOL_OP_BAND) return XR_OP_BAND_FLAG;
    if (symbol == SYMBOL_OP_BOR) return XR_OP_BOR_FLAG;
    if (symbol == SYMBOL_OP_BXOR) return XR_OP_BXOR_FLAG;
    if (symbol == SYMBOL_OP_BNOT) return XR_OP_BNOT_FLAG;
    if (symbol == SYMBOL_OP_LSHIFT) return XR_OP_LSHIFT_FLAG;
    if (symbol == SYMBOL_OP_RSHIFT) return XR_OP_RSHIFT_FLAG;
    if (symbol == SYMBOL_OP_INDEX) return XR_OP_INDEX_FLAG;
    if (symbol == SYMBOL_OP_INDEX_SET) return XR_OP_INDEX_SET_FLAG;
    if (symbol == SYMBOL_OP_INC) return XR_OP_INC_FLAG;
    if (symbol == SYMBOL_OP_DEC) return XR_OP_DEC_FLAG;
    if (symbol == SYMBOL_OP_NOT) return XR_OP_NOT_FLAG;

    return 0;
}

// Compute operator overload flags (call once after class creation)
void xr_class_compute_operator_flags(XrClass *cls) {
    if (!cls) return;

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

static XrClass* xr_class_new_single(XrayIsolate *X, const char *name) {
    (void)X;
    XR_DCHECK(name != NULL, "Class name must not be NULL");

    XrClass *cls = XR_ALLOCATE(XrClass);
    if (!cls) return NULL;

    // Zero all fields first; XR_ALLOCATE (xr_malloc) does not clear memory.
    // Fields like field_default_values / struct_layout / reflect_cache /
    // type_metadata would otherwise hold garbage and trip xr_class_free.
    memset(cls, 0, sizeof(*cls));

    xr_gc_header_init_type(&cls->gc, XR_TCLASS);
    cls->name = xr_strdup(name);

    // Root class: primary_supers[0] == self, depth == 0
    cls->primary_supers[0] = cls;
    cls->instance_size = sizeof(XrGCHeader);

    return cls;
}

XrClass* xr_class_new(XrayIsolate *X, const char *name, XrClass *super) {
    XR_DCHECK(name != NULL, "Class name must not be NULL");

    XrClass *cls = xr_class_new_single(X, name);
    if (!cls) return NULL;

    if (super) {
        xr_class_set_super(cls, super);
    }

    // NOTE: callers are responsible for reflection registration. The core
    // isolate init does that in a batch after all builtin classes are ready
    // (see xr_core_init). Enum creation calls xr_registry_register_class()
    // directly right after this returns.
    return cls;
}

// Set superclass (establish inheritance)
void xr_class_set_super(XrClass *subclass, XrClass *superclass) {
    if (!subclass || !superclass) return;

    // Cannot inherit from a final class
    if (superclass->flags & XR_CLASS_FINAL) {
        xr_log_warning("class", "class '%s' cannot inherit from final class '%s'",
                       subclass->name ? subclass->name : "<anonymous>",
                       superclass->name ? superclass->name : "<anonymous>");
        return;
    }

    subclass->super = superclass;

    // Mark parent as having subclasses (CHA devirtualization)
    superclass->flags |= XR_CLASS_HAS_SUBCLASSES;

    // Inherit parent's operator flags
    subclass->operator_flags |= superclass->operator_flags;

    // Update primary supers array
    subclass->depth = superclass->depth + 1;

    if (subclass->depth < 8) {
        for (int i = 0; i <= superclass->depth; i++) {
            subclass->primary_supers[i] = superclass->primary_supers[i];
        }
        subclass->primary_supers[subclass->depth] = subclass;
        for (int i = subclass->depth + 1; i < 8; i++) {
            subclass->primary_supers[i] = NULL;
        }
    } else {
        // Depth >= 8: keep the 8 shallowest ancestors in
        // [Object, parent1, ..., parent7] order so that instanceof's
        // O(1) lookup remains correct for any target with depth < 8.
        // Deeper targets fall back to linear scan in xr_class_instanceof
        // (will be replaced by a secondary supers hash in P10).
        XrClass *chain[256];
        int n = 0;
        for (XrClass *c = subclass; c != NULL && n < 256; c = c->super) {
            chain[n++] = c;
        }
        // chain[n-1] is Object, chain[0] is subclass itself.
        for (int i = 0; i < 8 && n - 1 - i >= 0; i++) {
            subclass->primary_supers[i] = chain[n - 1 - i];
        }
    }

    // Update field layout
    int parent_instance_field_count = xr_class_instance_field_count(superclass);
    int own_instance_fields = subclass->own_field_count - subclass->static_field_count;
    int old_instance_field_count = xr_class_instance_field_count(subclass);

    subclass->field_count = parent_instance_field_count + subclass->own_field_count;

    // Resize field_default_values when inherited fields increase the total
    int new_instance_field_count = parent_instance_field_count + own_instance_fields;
    if (new_instance_field_count > old_instance_field_count) {
        XrValue *new_defaults = (XrValue*)xr_malloc(
            new_instance_field_count * sizeof(XrValue));
        if (new_defaults) {
            // Parent slots: copy from superclass defaults or null
            for (int i = 0; i < parent_instance_field_count; i++) {
                new_defaults[i] = (superclass->field_default_values && i < (int)xr_class_instance_field_count(superclass))
                    ? superclass->field_default_values[i] : xr_null();
            }
            // Own slots: copy from old defaults or null
            for (int i = 0; i < own_instance_fields; i++) {
                int new_idx = parent_instance_field_count + i;
                new_defaults[new_idx] = (subclass->field_default_values && i < old_instance_field_count)
                    ? subclass->field_default_values[i] : xr_null();
            }
            if (subclass->field_default_values)
                xr_free(subclass->field_default_values);
            subclass->field_default_values = new_defaults;
        }
    }

    // Rebuild symbol mapping
    if (subclass->field_symbol_to_index && subclass->own_field_count > 0) {
        for (int i = 0; i < subclass->field_map_capacity; i++) {
            subclass->field_symbol_to_index[i] = -1;
        }

        int field_base_index = parent_instance_field_count;
        for (int i = 0; i < own_instance_fields; i++) {
            int global_idx = field_base_index + i;
            subclass->field_symbol_to_index[subclass->fields[i].symbol] = global_idx;
        }

        int total_instance_fields = parent_instance_field_count + own_instance_fields;
        for (int i = 0; i < subclass->static_field_count; i++) {
            int field_idx = own_instance_fields + i;
            subclass->field_symbol_to_index[subclass->fields[field_idx].symbol] =
                total_instance_fields + i;
        }
    }

    subclass->instance_size = superclass->instance_size + own_instance_fields * sizeof(void*);

    // Flatten parent instance methods into subclass methods[] array.
    // At this point, subclass->methods[] may only contain own methods (from CLASS_FROM_DESC
    // with Object as placeholder super). Now rebuild with the real superclass.
    {
        int parent_mc = superclass->method_count;  // parent instance methods (flattened)
        int own_mc = subclass->method_count;        // current own methods
        int own_static = subclass->static_method_count;

        // Count overrides (O(n) via parent's symbol-to-index table)
        int override_cnt = 0;
        if (superclass->method_symbol_to_index) {
            int cap = superclass->method_map_capacity;
            for (int i = 0; i < own_mc; i++) {
                if (subclass->methods[i].flags & XMETHOD_FLAG_STATIC) continue;
                int sym = subclass->methods[i].symbol;
                if (sym >= 0 && sym < cap && superclass->method_symbol_to_index[sym] >= 0
                    && superclass->method_symbol_to_index[sym] < parent_mc) {
                    override_cnt++;
                }
            }
        }

        // Count own non-static methods that are NOT overrides
        int own_instance = 0;
        for (int i = 0; i < own_mc; i++) {
            if (!(subclass->methods[i].flags & XMETHOD_FLAG_STATIC)) own_instance++;
        }
        int new_own = own_instance - override_cnt;
        int flat_count = parent_mc + new_own;
        int total = flat_count + own_static;

        if (total > 0 && parent_mc > 0) {
            // Save own methods to temp buffer
            XrMethod *own_methods = (XrMethod*)xr_malloc(own_mc * sizeof(XrMethod));
            memcpy(own_methods, subclass->methods, own_mc * sizeof(XrMethod));

            // Reallocate methods array for flattened layout
            XrMethod *new_methods = (XrMethod*)xr_malloc(total * sizeof(XrMethod));

            // Step 1: copy parent instance methods
            for (int i = 0; i < parent_mc; i++) {
                new_methods[i] = superclass->methods[i];
                if (superclass->methods[i].name) {
                    new_methods[i].name = xr_strdup(superclass->methods[i].name);
                }
            }

            // Step 2: apply own instance methods (override or append)
            int append_idx = parent_mc;
            for (int i = 0; i < own_mc; i++) {
                if (own_methods[i].flags & XMETHOD_FLAG_STATIC) continue;

                // O(1) override lookup via parent's symbol table
                int slot = -1;
                if (superclass->method_symbol_to_index) {
                    int sym = own_methods[i].symbol;
                    if (sym >= 0 && sym < superclass->method_map_capacity) {
                        int idx = superclass->method_symbol_to_index[sym];
                        if (idx >= 0 && idx < parent_mc) slot = idx;
                    }
                }

                if (slot >= 0) {
                    // Override: free inherited name, replace
                    if (new_methods[slot].name) xr_free((void*)new_methods[slot].name);
                    new_methods[slot] = own_methods[i];
                    // own_methods[i].name ownership transferred, don't free
                } else {
                    new_methods[append_idx] = own_methods[i];
                    append_idx++;
                }
            }

            // Step 3: copy static methods at the end
            int static_idx = flat_count;
            for (int i = 0; i < own_mc; i++) {
                if (own_methods[i].flags & XMETHOD_FLAG_STATIC) {
                    new_methods[static_idx++] = own_methods[i];
                }
            }

            xr_free(subclass->methods);
            subclass->methods = new_methods;
            subclass->method_count = flat_count;
            // static_method_count stays the same

            xr_free(own_methods);

            // Rebuild method_symbol_to_index
            if (subclass->method_symbol_to_index) {
                xr_free(subclass->method_symbol_to_index);
                subclass->method_symbol_to_index = NULL;
            }
            int all_count = flat_count + own_static;
            if (all_count > 0) {
                int max_sym = 0;
                for (int i = 0; i < all_count; i++) {
                    if (subclass->methods[i].symbol > max_sym)
                        max_sym = subclass->methods[i].symbol;
                }
                subclass->method_map_capacity = max_sym + 1;
                subclass->method_symbol_to_index = (int*)xr_malloc(
                    subclass->method_map_capacity * sizeof(int));
                for (int i = 0; i < subclass->method_map_capacity; i++)
                    subclass->method_symbol_to_index[i] = -1;
                for (int i = 0; i < all_count; i++) {
                    int sym = subclass->methods[i].symbol;
                    if (sym >= 0 && sym < subclass->method_map_capacity)
                        subclass->method_symbol_to_index[sym] = i;
                }
            }
        }
    }

    // Rebuild vtable (inherit from parent)
    if (subclass->vtable) {
        xr_free(subclass->vtable);
        subclass->vtable = NULL;
        subclass->vtable_size = 0;
    }

    if (superclass->vtable && superclass->vtable_size > 0) {
        subclass->vtable_size = superclass->vtable_size;
        subclass->vtable = (XrMethod**)xr_malloc(subclass->vtable_size * sizeof(XrMethod*));
        memcpy(subclass->vtable, superclass->vtable, subclass->vtable_size * sizeof(XrMethod*));
        subclass->own_method_start = superclass->vtable_size;
    } else {
        subclass->own_method_start = 0;
    }

    // Process subclass methods (override or add)
    for (int i = 0; i < subclass->method_count; i++) {
        XrMethod *method = &subclass->methods[i];

        if (method->flags & XMETHOD_FLAG_STATIC) {
            method->vtable_index = -1;
            continue;
        }

        int parent_vtable_idx = -1;
        if (superclass->vtable) {
            for (int j = 0; j < superclass->vtable_size; j++) {
                if (superclass->vtable[j] && superclass->vtable[j]->symbol == method->symbol) {
                    parent_vtable_idx = j;
                    break;
                }
            }
        }

        if (parent_vtable_idx >= 0) {
            // Cannot override a final method
            XrMethod *parent_method = superclass->vtable[parent_vtable_idx];
            if (parent_method && (parent_method->flags & XMETHOD_FLAG_FINAL)) {
                xr_log_warning("class",
                               "method '%s' in class '%s' cannot override final method from '%s'",
                               method->name ? method->name : "<unknown>",
                               subclass->name ? subclass->name : "<anonymous>",
                               superclass->name ? superclass->name : "<anonymous>");
                continue;
            }
            // Override parent method
            subclass->vtable[parent_vtable_idx] = method;
            method->vtable_index = parent_vtable_idx;
        } else {
            // New method: extend vtable
            if (subclass->vtable) {
                XrMethod **new_vt = (XrMethod**)xr_realloc(subclass->vtable, (subclass->vtable_size + 1) * sizeof(XrMethod*));
                if (!new_vt) continue;
                subclass->vtable = new_vt;
            } else {
                subclass->vtable = (XrMethod**)xr_malloc(sizeof(XrMethod*));
                if (!subclass->vtable) continue;
            }
            subclass->vtable[subclass->vtable_size] = method;
            method->vtable_index = subclass->vtable_size;
            subclass->vtable_size++;
        }
    }

    // Rebuild ITable (if interfaces exist)
    xr_class_build_itable(subclass);

    xr_class_inherit_abstract_methods(subclass, superclass);
}

const XrFieldDescriptor* xr_class_get_field(const XrClass *cls, int index) {
    if (!cls || index < 0 || index >= cls->field_count) {
        return NULL;
    }
    XR_DCHECK(cls->fields != NULL, "class_get_field: NULL fields array");
    return &cls->fields[index];
}

int xr_class_lookup_field_by_name(XrClass *cls, const char *name) {
    if (!cls || !name) return -1;

    for (int i = 0; i < cls->field_count; i++) {
        if (cls->fields && cls->fields[i].name &&
            strcmp(cls->fields[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

// O(1) lookup + O(depth) recursive parent lookup
int xr_class_lookup_field(XrClass *cls, int symbol) {
    if (!cls || symbol < 0) return -1;

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
XrMethod* xr_class_lookup_method(XrClass *cls, int symbol) {
    if (!cls || symbol < 0) return NULL;

    if (cls->method_symbol_to_index && symbol < cls->method_map_capacity) {
        int method_idx = cls->method_symbol_to_index[symbol];

        if (method_idx >= 0 && method_idx < cls->method_count) {
            XrMethod *method = &cls->methods[method_idx];

            if (method->symbol == symbol && method->type != XMETHOD_NONE) {
                // VTable optimization for closure methods only;
                // primitive methods do not use vtable dispatch.
                if (method->type != XMETHOD_PRIMITIVE &&
                    method->vtable_index >= 0 &&
                    cls->vtable &&
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
XrClass* xr_value_get_class(XrayIsolate *X, XrValue value) {
    XR_DCHECK(X != NULL, "value_get_class: NULL isolate");
    if (XR_IS_PTR(value)) {
        XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(value);
        XrObjType type = XR_GC_GET_TYPE(gc);

        if (type == XR_TINSTANCE) {
            XrInstance *inst = (XrInstance*)gc;
            return inst->klass;
        }

        if (type == XR_TENUM_VALUE) {
            XrEnumValue *ev = (XrEnumValue*)gc;
            if (ev->enum_name) {
                XrClass *cls = xr_class_lookup_by_name(X, ev->enum_name);
                if (cls) return cls;
            }
            XrayCoreClasses *core = xr_isolate_get_core_classes(X);
            return core ? core->enumClass : NULL;
        }

        if (type == XR_TENUM_TYPE) {
            XrEnumType *et = (XrEnumType*)gc;
            return et->enum_class;
        }

        XrayCoreClasses *_core = xr_isolate_get_core_classes(X);
        if (_core) {
            switch (type) {
                case XR_TARRAY_SLICE:
                    return _core->arraySliceClass;
                case XR_TSTRINGBUILDER:
                    return _core->stringBuilderClass;
                default:
                    break;
            }
        }

        return NULL;
    }

    XrayCoreClasses *core = xr_isolate_get_core_classes(X);
    if (!core) {
        return NULL;
    }

    if (XR_IS_INT(value)) {
        return core->intClass;
    }
    if (XR_IS_FLOAT(value)) {
        return core->floatClass;
    }
    if (XR_IS_BOOL(value)) {
        return core->boolClass;
    }
    if (XR_IS_NULL(value)) {
        return core->nullClass;
    }

    if (XR_IS_PTR(value)) {
        XrGCHeader *gc = (XrGCHeader*)XR_TO_PTR(value);
        XrObjType type = XR_GC_GET_TYPE(gc);

        switch (type) {
            case XR_TARRAY_SLICE:
                return core->arraySliceClass;
            default:
                break;
        }
    }

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
        if (cls->methods[i].type != XMETHOD_NONE) method_count++;
    }
    printf("  Methods: %d (array size: %d)\n", method_count, cls->method_count);
    printf("  Static methods: %d\n", cls->static_method_count);
    printf("}\n");
}

// Static and instance methods stored together
// Use xr_class_lookup_method() for all methods
// Check method->flags & XMETHOD_FLAG_STATIC for static methods

/* ========== Interface Support ========== */

XrClass* xr_interface_new(XrayIsolate *X, const char *name) {
    XR_DCHECK(X != NULL, "interface_new: NULL isolate");
    XR_DCHECK(name != NULL, "Interface name must not be NULL");

    XrClass *iface = xr_class_new_single(X, name);
    iface->flags |= XR_CLASS_INTERFACE;

    return iface;
}

bool xr_class_implements_interface(XrClass *cls, const char *interface_name) {
    if (!cls || !interface_name) return false;

    for (int i = 0; i < cls->interface_count; i++) {
        if (cls->interfaces[i] && cls->interfaces[i]->name &&
            strcmp(cls->interfaces[i]->name, interface_name) == 0) {
            return true;
        }
    }

    if (cls->super) {
        return xr_class_implements_interface(cls->super, interface_name);
    }

    return false;
}

bool xr_class_has_method(XrClass *cls, int method_symbol) {
    if (!cls || method_symbol < 0) return false;

    XrMethod *method = xr_class_lookup_method(cls, method_symbol);
    return (method != NULL && method->type != XMETHOD_NONE);
}

// Fast interface check (pointer compare, walks inheritance chain)
bool xr_class_implements_interface_fast(XrClass *cls, XrClass *iface) {
    if (!cls || !iface || !(iface->flags & XR_CLASS_INTERFACE)) return false;

    for (XrClass *cur = cls; cur != NULL; cur = cur->super) {
        for (int i = 0; i < cur->interface_count; i++) {
            if (cur->interfaces[i] == iface) {
                return true;
            }
        }
    }
    return false;
}

int xr_class_verify_interface(XrClass *cls, XrClass *iface,
                               char **errors, int max_errors) {
    if (!cls || !iface) return 0;
    if (!(iface->flags & XR_CLASS_INTERFACE)) return 0;

    int satisfied_count = 0;
    int error_count = 0;

    for (int i = 0; i < iface->method_count; i++) {
        XrMethod *iface_method = &iface->methods[i];

        if (iface_method->type == XMETHOD_NONE) {
            continue;
        }

        if (xr_class_has_method(cls, iface_method->symbol)) {
            satisfied_count++;
        } else {
            if (errors && error_count < max_errors) {
                errors[error_count] = xr_strdup("unknown_method");
                error_count++;
            }
        }
    }

    return satisfied_count;
}

/* ========== ITable Generation ========== */

int xr_class_build_itable(XrClass *cls) {
    if (!cls) return -1;

    // Free existing itable
    if (cls->itable) {
        for (int i = 0; i < cls->itable_size; i++) {
            if (cls->itable[i].methods) {
                xr_free(cls->itable[i].methods);
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
    cls->itable = (XrItableEntry*)xr_malloc(cls->itable_size * sizeof(XrItableEntry));
    if (!cls->itable) {
        return -1;
    }

    for (int i = 0; i < cls->interface_count; i++) {
        XrClass *iface = cls->interfaces[i];
        if (!iface) continue;

        cls->itable[i].interface = iface;
        cls->itable[i].method_count = iface->method_count;

        if (iface->method_count > 0) {
            cls->itable[i].methods = (XrMethod**)xr_malloc(
                iface->method_count * sizeof(XrMethod*));
            if (!cls->itable[i].methods) return -1;

            for (int j = 0; j < iface->method_count; j++) {
                XrMethod *iface_method = &iface->methods[j];
                XrMethod *impl = NULL;

                for (int k = 0; k < cls->method_count; k++) {
                    if (cls->methods[k].symbol == iface_method->symbol) {
                        impl = &cls->methods[k];
                        break;
                    }
                }
                cls->itable[i].methods[j] = impl;
            }
        } else {
            cls->itable[i].methods = NULL;
        }
    }

    return 0;
}

/* ========== Abstract Class Support ========== */

void xr_class_mark_abstract(XrClass *cls) {
    if (!cls) return;
    cls->flags |= XR_CLASS_ABSTRACT;
}

void xr_class_add_abstract_method(XrClass *cls, int method_symbol) {
    if (!cls || method_symbol < 0) return;

    int new_count = cls->abstract_method_count + 1;
    int *new_methods = (int*)xr_realloc(cls->abstract_methods, sizeof(int) * new_count);
    if (!new_methods) return;
    cls->abstract_methods = new_methods;
    cls->abstract_method_count = new_count;
    cls->abstract_methods[new_count - 1] = method_symbol;
}

// Check if class can be instantiated (not abstract and all abstract methods implemented)
bool xr_class_can_instantiate(XrClass *cls) {
    if (!cls) return false;

    if (cls->flags & XR_CLASS_ABSTRACT) return false;

    for (int i = 0; i < cls->abstract_method_count; i++) {
        int symbol = cls->abstract_methods[i];
        XrMethod *method = xr_class_lookup_method(cls, symbol);

        if (!method || method->type == XMETHOD_NONE ||
            xr_method_is_abstract(method)) {
            return false;
        }
    }

    return true;
}

// Inherit abstract methods from parent
void xr_class_inherit_abstract_methods(XrClass *child, XrClass *parent) {
    if (!child || !parent) return;

    if (parent->abstract_method_count == 0) return;

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
    if (!cls || method_symbol < 0) return false;

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

    XrGCHeader *gc = (XrGCHeader*)obj;
    XrObjType type = XR_GC_GET_TYPE(gc);

    if (type != XR_TINSTANCE) {
        return false;
    }

    XrInstance *inst = (XrInstance*)obj;
    XrClass *obj_class = inst->klass;

    if (obj_class == NULL) {
        return false;
    }

    return xr_class_instanceof(obj_class, target);
}

/* ========== ITable Interface Method Lookup ========== */

// O(n) where n = interface count (usually < 5)
XrMethod* xr_class_lookup_interface_method(XrClass *cls, XrClass *iface, int method_index) {
    if (!cls || !iface || !cls->itable || method_index < 0) {
        return NULL;
    }

    for (int i = 0; i < cls->itable_size; i++) {
        if (cls->itable[i].interface == iface) {
            if (method_index < cls->itable[i].method_count) {
                return cls->itable[i].methods[method_index];
            }
            return NULL;
        }
    }

    return NULL;
}

// Lookup by symbol when method index is unknown at compile time
XrMethod* xr_class_lookup_interface_method_by_symbol(XrClass *cls, XrClass *iface, int method_symbol) {
    if (!cls || !iface || !cls->itable) {
        return NULL;
    }

    for (int i = 0; i < cls->itable_size; i++) {
        if (cls->itable[i].interface == iface) {
            for (int j = 0; j < cls->itable[i].method_count; j++) {
                XrMethod *method = cls->itable[i].methods[j];
                if (method && method->symbol == method_symbol) {
                    return method;
                }
            }
            return NULL;
        }
    }

    return NULL;
}

/*
 * Free class resources
 * Note: Classes are typically GC-managed; this is for explicit cleanup during error paths
 */
void xr_class_free(XrClass *cls) {
    if (!cls) return;

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

    // Free itable entries
    if (cls->itable) {
        for (int i = 0; i < cls->itable_size; i++) {
            if (cls->itable[i].methods) {
                xr_free(cls->itable[i].methods);
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

    // Class name is interned string, not freed here

    xr_free(cls);
}

