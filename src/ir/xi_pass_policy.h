/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_pass_policy.h - Optimizer policy for a compilation session
 *
 * KEY CONCEPT:
 *   Which Xi optimization passes run is a property of the compilation
 *   session, not of a module. One session resolves one policy and every
 *   module it compiles reads that same policy, so a function inlined across
 *   a module boundary was optimized under the same rules as its new host.
 *   A per-module knob could not state which rules the inlined body followed.
 *
 *   A policy carries one entry per pipeline. The VM and the native backend
 *   run different pass sets by design, so each is addressed separately:
 *   the VM defaults to the light level, the native backend to the full
 *   level minus the passes named in the built-in default below.
 *
 *   This is a diagnostic and containment mechanism. It exists so a defective
 *   pass can be named and switched off with the reason recorded, and so a
 *   divergence can be bisected pass by pass. It is not an optimization
 *   strategy offered to programs: the built-in default is the supported
 *   configuration and every deviation is a stated, recorded exception.
 *
 * SPEC GRAMMAR (accepted by --xi-opt and by XRAY_XI_OPT):
 *   spec  := entry (',' entry)*
 *   entry := pipeline '=' level ('-' pass)*
 *   pipeline := "vm" | "aot"
 *   level    := "none" | "light" | "full"
 *   pass     := a name from XI_OPT_PASS_LIST
 *
 *   "vm=full"                       run the VM at the level the AOT uses
 *   "aot=full-ifconv-loop_split"    full minus two passes
 *   "vm=full,aot=light"             both pipelines in one spec
 *
 *   Every token is checked. An unknown pipeline, level or pass name is an
 *   error that stops the session; nothing falls back to a default level.
 */

#ifndef XI_PASS_POLICY_H
#define XI_PASS_POLICY_H

#include "xi_pass.h"
#include <stddef.h>

/* The two pipelines that select passes independently. */
typedef enum {
    XI_OPT_PIPELINE_VM = 0,
    XI_OPT_PIPELINE_AOT,
    XI_OPT_PIPELINE_COUNT
} XiOptPipelineId;

typedef struct XiOptPipelinePolicy {
    XiOptLevel level;
    XiOptDisableMask disabled; /* passes withheld from `level` */
} XiOptPipelinePolicy;

typedef struct XiOptPolicy {
    XiOptPipelinePolicy pipelines[XI_OPT_PIPELINE_COUNT];
} XiOptPolicy;

/* Longest rendering of a policy, including the terminator. Both pipelines,
 * every pass name, both separators. */
#define XI_PASS_POLICY_TEXT_MAX 640

/* ========== Pass and level names ========== */

/* Pass name for a XiOptPassId, or NULL when the id is out of range. This is
 * the single name table; xi_opt.c checks the pass table against it at
 * startup so a mask bit can never address a different pass than its name. */
XR_FUNC const char *xi_pass_name_by_id(int pass_id);

/* Pass id for a name, or -1 when no pass carries that name. */
XR_FUNC int xi_pass_id_by_name(const char *name);

/* Level name ("none", "light", "full"), or NULL for an unknown level. */
XR_FUNC const char *xi_pass_level_name(XiOptLevel level);

/* Pipeline name ("vm", "aot"), or NULL for an unknown pipeline. */
XR_FUNC const char *xi_pass_pipeline_name(XiOptPipelineId pipeline);

/* ========== Policies ========== */

/* The configuration the compiler ships with. */
XR_FUNC XiOptPolicy xi_pass_policy_builtin_default(void);

/* Apply `spec` on top of `policy`. Returns false with a message in `err` and
 * leaves `policy` untouched when any token is unknown or malformed; there is
 * no partial application and no fallback level. */
XR_FUNC bool xi_pass_policy_apply_spec(XiOptPolicy *policy, const char *spec, char *err,
                                       size_t err_size);

/* Render a policy as a single deterministic line in spec syntax. This is the
 * provenance form: it round-trips through xi_pass_policy_apply_spec. Returns
 * false only when the buffer is too small. */
XR_FUNC bool xi_pass_policy_render(const XiOptPolicy *policy, char *buf, size_t buf_size);

/* True when the policy is exactly the built-in default. */
XR_FUNC bool xi_pass_policy_is_builtin_default(const XiOptPolicy *policy);

/* ========== Session policy ========== */

/* The policy in force for this compilation session. The first call resolves
 * XRAY_XI_OPT; a malformed value aborts the session rather than selecting a
 * level nobody asked for. */
XR_FUNC const XiOptPolicy *xi_pass_session_policy(void);

/* The entry a pipeline configuration constructor reads. Reading seals the
 * session policy: a later change would mean two modules of one build were
 * optimized under different rules. */
XR_FUNC XiOptPipelinePolicy xi_pass_session_pipeline_policy(XiOptPipelineId pipeline);

/* Apply `spec` to the session policy on behalf of `origin` (the command-line
 * option name, used in error text). Returns false with a message in `err`
 * when the spec is malformed or when the session policy has already been
 * read by a pipeline. */
XR_FUNC bool xi_pass_session_policy_apply_spec(const char *spec, const char *origin, char *err,
                                               size_t err_size);

/* Reset the session policy to the built-in default and unseal it. For tests
 * that need to exercise several policies in one process. */
XR_FUNC void xi_pass_session_policy_reset_for_testing(void);

#endif /* XI_PASS_POLICY_H */
