/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xclass_builder.h - Class builder API for compile/load time
 *
 * KEY CONCEPT:
 *   Dynamic arrays for collecting fields/methods during build.
 *   Optimizes and freezes into immutable class on finalize.
 */

#ifndef XCLASS_BUILDER_H
#define XCLASS_BUILDER_H

#include "xclass.h"
#include <xray_isolate.h>
#include <stdbool.h>

/*
 * XrClassBuilder is an opaque, transient scratchpad used at class
 * build time. The full layout (struct XrClassBuilder plus the
 * XrFieldBuildItem / XrMethodBuildItem / XrStaticFieldBuildItem
 * scaffolding) lives in xclass_builder_internal.h and must not be
 * accessed from outside the class/ subsystem.
 *
 * Callers only ever deal with the handle returned by
 * xr_class_builder_new(), feed it through the add_* helpers below,
 * and finish with xr_class_builder_finalize(), which frees the
 * builder and hands back an immutable XrClass.
 */

/* ========== Builder API ========== */

XR_FUNC XrClassBuilder *xr_class_builder_new(XrayIsolate *isolate, const char *name,
                                             XrClass *super);

// Returns 0 on success, -1 on failure (e.g., duplicate field)
XR_FUNC int xr_class_builder_add_field(XrClassBuilder *builder, const char *name, uint32_t flags);

// Returns 0 on success, -1 on failure (e.g., duplicate method)
XR_FUNC int xr_class_builder_add_method(XrClassBuilder *builder, const char *name,
                                        XrPrimitiveMethodFn impl, int param_count, uint32_t flags);

// Add method with closure (for descriptor path)
XR_FUNC int xr_class_builder_add_method_closure(XrClassBuilder *builder, const char *name,
                                                XrClosure *closure, XrMethodType method_type,
                                                int param_count, uint32_t flags, uint8_t op_type);

XR_FUNC int xr_class_builder_add_static_field(XrClassBuilder *builder, const char *name,
                                              XrValue value, uint32_t flags);

XR_FUNC int xr_class_builder_add_static_method(XrClassBuilder *builder, const char *name,
                                               XrPrimitiveMethodFn impl, int param_count,
                                               uint32_t flags);

// Add static method with closure (for descriptor path)
XR_FUNC int xr_class_builder_add_static_method_closure(XrClassBuilder *builder, const char *name,
                                                       XrClosure *closure, int param_count,
                                                       uint32_t flags);

XR_FUNC int xr_class_builder_add_interface(XrClassBuilder *builder, XrClass *interface);

XR_FUNC int xr_class_builder_add_abstract_method(XrClassBuilder *builder, int method_symbol);

XR_FUNC void xr_class_builder_set_flags(XrClassBuilder *builder, uint32_t flags);

/* Monomorphized generics: set before finalize to populate the
 * class's generic_origin, display_name and reified type args. */
XR_FUNC void xr_class_builder_set_display_name(XrClassBuilder *builder, const char *display_name);
XR_FUNC void xr_class_builder_set_generic_origin(XrClassBuilder *builder, XrClass *origin);
XR_FUNC void xr_class_builder_set_mono_type_arg_names(XrClassBuilder *builder,
                                                       const char **type_arg_names, uint8_t argc);

// Finalize: compute offsets, generate vtable, freeze class, destroy builder
XR_FUNC XrClass *xr_class_builder_finalize(XrClassBuilder *builder);

// Manual cleanup if finalize fails
XR_FUNC void xr_class_builder_destroy(XrClassBuilder *builder);

/* ========== Duplicate Detection ========== */

XR_FUNC bool xr_class_builder_has_field(const XrClassBuilder *builder, const char *name);
XR_FUNC bool xr_class_builder_has_method(const XrClassBuilder *builder, const char *name);

// calculate_instance_size and generate_vtable were removed when
// finalize was split into xclass_builder_finalize.c. The latter
// keeps vtable generation as a file-local static helper; no
// consumer outside finalize ever needed it.

#endif  // XCLASS_BUILDER_H
