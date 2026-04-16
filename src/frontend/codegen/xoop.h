/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xoop.h - OOP compiler (class, interface, enum)
 */

#ifndef XOOP_H
#define XOOP_H

#include "../parser/xast.h"
#include "xcompiler_emit.h"
#include "../../base/xdefs.h"

typedef struct XrCompilerContext XrCompilerContext;
typedef struct XrCompiler XrCompiler;

XR_FUNC void compile_class(XrCompilerContext *ctx, XrCompiler *c, ClassDeclNode *node);
XR_FUNC void compile_interface(XrCompilerContext *ctx, XrCompiler *c, InterfaceDeclNode *node);
XR_FUNC void compile_enum_decl(XrCompilerContext *ctx, XrCompiler *c, EnumDeclNode *node);

#endif // XOOP_H
