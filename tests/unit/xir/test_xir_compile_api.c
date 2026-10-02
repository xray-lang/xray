/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_xir_compile_api.c - Independent compile-owner ABI declarations
 */
#include "xir/xxir.h"
#include "xir/xxir_checked.h"
#include "xir/xxir_generic.h"
#include "xir/xxir_program.h"
#include "xir/xxir_type_arena.h"
#include "xir/xxir_vm.h"
#include "xir/xxir_emit_c.h"
#include <stddef.h>

typedef XrXirStatus (*Check)(const XrXirCompileContext *,const XrXirModule *,XrXirArtifact **,XrXirDiagnostic *);
typedef XrXirStatus (*Lower)(const XrXirArtifact *,const XrXirTarget *,XrXirArtifact **,XrXirDiagnostic *);
typedef XrXirStatus (*Specialize)(const XrXirArtifact *,XrXirArtifact **,XrXirDiagnostic *);
typedef XrXirStatus (*Seal)(const XrXirCompileContext *,const XrXirProgramSpec *,XrXirProgram **);
typedef XrXirValueStatus (*ArenaNew)(const XrXirCompileContext *,const XrXirTypes *,XrXirTypeArena **);
typedef XrXirStatus (*Read)(const XrXirCompileContext *,const void *,size_t,XrXirArtifact **,XrXirDiagnostic *);
typedef XrXirStatus (*Write)(const XrXirArtifact *,XrXirCheckedPacket *,XrXirDiagnostic *);
typedef XrXirStatus (*Take)(XrXirArtifact **,XrXirProgram **);
typedef XrXirStatus (*Emit)(const XrXirArtifact *,const char *,size_t,XrXirCSource *);
#define SIGNATURE(name,type) _Static_assert(_Generic(&(name),type:1,default:0),#name " signature")
SIGNATURE(xr_xir_compile_check,Check);
SIGNATURE(xr_xir_compile_lower,Lower);
SIGNATURE(xr_xir_compile_specialize,Specialize);
SIGNATURE(xr_xir_compile_program_seal,Seal);
SIGNATURE(xr_xir_compile_type_arena_new,ArenaNew);
SIGNATURE(xr_xir_compile_checked_read,Read);
SIGNATURE(xr_xir_compile_checked_write,Write);
SIGNATURE(xr_xir_compile_vm_program_take,Take);
SIGNATURE(xr_xir_compile_emit_c,Emit);
_Static_assert(sizeof(XrXirCompileLimits)==24 && _Alignof(XrXirCompileLimits)==8,"compile limits layout");
_Static_assert(offsetof(XrXirCompileLimits,functions)==0 && offsetof(XrXirCompileLimits,parameters)==4 &&
    offsetof(XrXirCompileLimits,blocks)==8 && offsetof(XrXirCompileLimits,instructions)==12 &&
    offsetof(XrXirCompileLimits,frame_bytes)==16,"compile limits offsets");
_Static_assert(sizeof(XrXirCompileContext)==32 && _Alignof(XrXirCompileContext)==8 &&
    offsetof(XrXirCompileContext,resources)==0 && offsetof(XrXirCompileContext,limits)==8,"compile context layout");
_Static_assert(XR_XIR_PROGRAM_ABI_VERSION==27 && XR_XIR_CALL_ABI_VERSION==22 &&
    XR_XIR_VALUE_ABI_VERSION==18,"compile owner admission ABI");
int main(void) { return 0; }
