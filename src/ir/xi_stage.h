/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_stage.h - Opaque, consuming, verified Xi stage transitions
 */

#ifndef XI_STAGE_H
#define XI_STAGE_H

#include "xi.h"
#include "../base/xdefs.h"
#include <stddef.h>

typedef struct XiRawProgram XiRawProgram;
typedef struct XiCanonicalProgram XiCanonicalProgram;
typedef struct XiClosedProgram XiClosedProgram;
typedef struct XiOwnedProgram XiOwnedProgram;
typedef struct XiSemanticLoweredProgram XiSemanticLoweredProgram;
typedef struct XiCoroLoweredProgram XiCoroLoweredProgram;
typedef struct XiOptimizedProgram XiOptimizedProgram;
typedef struct XiSemanticPlannedProgram XiSemanticPlannedProgram;
typedef struct XiReppedProgram XiReppedProgram;
typedef struct XiBackendProgram XiBackendProgram;

XR_FUNC XiRawProgram *xi_stage_adopt_raw(XiFunc *graph, char *error, size_t error_size);
XR_FUNC XiOptimizedProgram *xi_stage_adopt_optimized(XiFunc *graph, char *error, size_t error_size);
XR_FUNC XiSemanticPlannedProgram *xi_stage_adopt_semantic_planned(XiFunc *graph, char *error,
                                                                  size_t error_size);
XR_FUNC XiReppedProgram *xi_stage_adopt_repped(XiFunc *graph, char *error, size_t error_size);
XR_FUNC XiCanonicalProgram *xi_program_canonicalize(XiRawProgram *input, char *error,
                                                    size_t error_size);
XR_FUNC XiClosedProgram *xi_program_close(XiCanonicalProgram *input, char *error,
                                          size_t error_size);
XR_FUNC XiOwnedProgram *xi_program_make_owned(XiClosedProgram *input, char *error,
                                              size_t error_size);
XR_FUNC XiSemanticLoweredProgram *xi_program_lower_semantics(XiOwnedProgram *input, char *error,
                                                             size_t error_size);
XR_FUNC XiCoroLoweredProgram *xi_program_lower_coroutines(XiSemanticLoweredProgram *input,
                                                          const struct XiCoroResolver *resolver,
                                                          char *error, size_t error_size);
XR_FUNC XiOptimizedProgram *xi_program_finish_optimization(XiCoroLoweredProgram *input, char *error,
                                                           size_t error_size);
XR_FUNC XiSemanticPlannedProgram *xi_program_freeze_semantics(XiOptimizedProgram *input,
                                                              char *error, size_t error_size);
XR_FUNC XiReppedProgram *xi_program_select_reps(XiSemanticPlannedProgram *input, char *error,
                                                size_t error_size);
XR_FUNC XiBackendProgram *xi_program_plan_backend(XiReppedProgram *input, char *error,
                                                  size_t error_size);

XR_FUNC XiFunc *xi_raw_program_graph(XiRawProgram *program);
XR_FUNC XiFunc *xi_canonical_program_graph(XiCanonicalProgram *program);
XR_FUNC XiFunc *xi_closed_program_graph(XiClosedProgram *program);
XR_FUNC XiFunc *xi_owned_program_graph(XiOwnedProgram *program);
XR_FUNC XiFunc *xi_semantic_lowered_program_graph(XiSemanticLoweredProgram *program);
XR_FUNC XiFunc *xi_coro_lowered_program_graph(XiCoroLoweredProgram *program);
XR_FUNC XiFunc *xi_optimized_program_graph(XiOptimizedProgram *program);
XR_FUNC XiFunc *xi_semantic_planned_program_graph(XiSemanticPlannedProgram *program);
XR_FUNC XiFunc *xi_repped_program_graph(XiReppedProgram *program);
XR_FUNC XiFunc *xi_backend_program_graph(XiBackendProgram *program);

XR_FUNC XiFunc *xi_raw_program_release(XiRawProgram *program);
XR_FUNC XiFunc *xi_canonical_program_release(XiCanonicalProgram *program);
XR_FUNC XiFunc *xi_closed_program_release(XiClosedProgram *program);
XR_FUNC XiFunc *xi_owned_program_release(XiOwnedProgram *program);
XR_FUNC XiFunc *xi_semantic_lowered_program_release(XiSemanticLoweredProgram *program);
XR_FUNC XiFunc *xi_coro_lowered_program_release(XiCoroLoweredProgram *program);
XR_FUNC XiFunc *xi_optimized_program_release(XiOptimizedProgram *program);
XR_FUNC XiFunc *xi_semantic_planned_program_release(XiSemanticPlannedProgram *program);
XR_FUNC XiFunc *xi_repped_program_release(XiReppedProgram *program);
XR_FUNC XiFunc *xi_backend_program_release(XiBackendProgram *program);

#endif  // XI_STAGE_H
