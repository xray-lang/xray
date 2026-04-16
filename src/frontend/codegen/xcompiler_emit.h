/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcompiler_emit.h - Compiler-level instruction emission macros
 *
 * KEY CONCEPT:
 *   Convenience macros that bridge (ctx, compiler) calling convention
 *   to the underlying emitter API which takes XrEmitter* directly.
 *   All macros use UPPER_CASE to avoid shadowing the actual functions.
 */

#ifndef XCOMPILER_EMIT_H
#define XCOMPILER_EMIT_H

#include "xemit.h"

// Core instruction formats
#define EMIT_ABC(ctx, c, op, a, b, cc) \
    ((void)(ctx), (emit_abc)((c)->emitter, op, a, b, cc))

#define EMIT_ABSC(ctx, c, op, a, b, sc) \
    ((void)(ctx), (emit_abc)((c)->emitter, op, a, b, (uint8_t)((sc) & 0xFF)))

#define EMIT_ABX(ctx, c, op, a, bx) \
    ((void)(ctx), (emit_abx)((c)->emitter, op, a, bx))

#define EMIT_ASBX(ctx, c, op, a, sbx) \
    ((void)(ctx), (emit_asbx)((c)->emitter, op, a, sbx))

// Jump and loop
#define EMIT_JUMP(ctx, c, op) \
    ((void)(ctx), (emit_jump)((c)->emitter, op))

#define EMIT_PATCH_JUMP(ctx, c, offset) \
    ((void)(ctx), (patch_jump)((c)->emitter, offset, -1))

#define EMIT_LOOP(ctx, c, loop_start) \
    ((void)(ctx), (emit_loop)((c)->emitter, loop_start))

// Common instructions
#define EMIT_MOVE(ctx, c, dst, src)        emit_move((c)->emitter, dst, src)
#define EMIT_LOADNULL(ctx, c, reg)         emit_loadnull((c)->emitter, reg)
#define EMIT_LOADTRUE(ctx, c, reg)         emit_loadtrue((c)->emitter, reg)
#define EMIT_LOADFALSE(ctx, c, reg)        emit_loadfalse((c)->emitter, reg)
#define EMIT_RETURN(ctx, c, reg, n)        emit_return((c)->emitter, reg, n)

#endif // XCOMPILER_EMIT_H
