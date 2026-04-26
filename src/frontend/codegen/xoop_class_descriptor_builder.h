/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xoop_class_descriptor_builder.h - ClassDescriptor builder (compiler helper functions)
 *
 * KEY CONCEPT:
 *   - Create ClassDescriptor from AST nodes
 *   - Collect fields, methods, interfaces info
 *   - Store ClassDescriptor in constant pool
 */

#ifndef XOOP_CLASS_DESCRIPTOR_BUILDER_H
#define XOOP_CLASS_DESCRIPTOR_BUILDER_H

#include "../../runtime/class/xclass_descriptor.h"
#include "xcompiler.h"
#include "xcompiler_context.h"
#include "../parser/xast.h"
#include "../../base/xdefs.h"

// ========== Main Functions ==========

/*
 * Create ClassDescriptor from AST node
 *
 * @param ctx       compiler context
 * @param compiler  compiler
 * @param node      class declaration AST node
 * @return          ClassDescriptor object (needs manual release)
 */
XR_FUNC XrClassDescriptor *xoop_create_class_descriptor(XrCompilerContext *ctx,
                                                        XrCompiler *compiler, ClassDeclNode *node);

/*
 * Store ClassDescriptor in constant pool
 *
 * @param proto     function prototype
 * @param desc      ClassDescriptor object
 * @return          index in constant pool
 *
 * Note: ClassDescriptor itself is stored as pointer (GC object)
 */
XR_FUNC int xoop_add_descriptor_to_constant_pool(XrProto *proto, XrClassDescriptor *desc);

/*
 * Release ClassDescriptor and its internal resources
 *
 * @param desc  ClassDescriptor object
 */
XR_FUNC void xoop_free_class_descriptor(XrClassDescriptor *desc);

#endif  // XOOP_CLASS_DESCRIPTOR_BUILDER_H
