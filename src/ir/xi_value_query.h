/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_value_query.h - Backend-neutral IR value/type classification predicates
 *
 * KEY CONCEPT:
 *   Classify an XiValue by the runtime type it carries (channel / task /
 *   work queue / unknown).  The value-level predicates unwrap identity casts
 *   (BOX/UNBOX/COPY) and the type-level predicates recurse through union
 *   members, so a value typed `Channel | null` is still recognized as a
 *   channel.
 *
 *   The type a value carries is an IR-level fact, so these belong in the IR
 *   layer where both the AOT compiler and the VM can consume them without
 *   each backend re-implementing the recognition logic.
 */

#ifndef XI_VALUE_QUERY_H
#define XI_VALUE_QUERY_H

#include "xi.h"

/* Type-level predicates (union-recursive). 'type' may be NULL. */
XR_FUNC bool xi_type_is_channel(const struct XrType *type);
XR_FUNC bool xi_type_is_named_instance(const struct XrType *type, const char *name);
XR_FUNC bool xi_type_is_task(const struct XrType *type);
XR_FUNC bool xi_type_is_thread(const struct XrType *type);

/* Value-level predicates: unwrap BOX/UNBOX/COPY, then test the carried type. */
XR_FUNC bool xi_value_type_is_channel(const XiValue *v);
XR_FUNC bool xi_value_type_is_task(const XiValue *v);
XR_FUNC bool xi_value_type_is_thread(const XiValue *v);
XR_FUNC bool xi_value_type_is_atomic(const XiValue *v);
XR_FUNC bool xi_value_type_is_work_queue(const XiValue *v);
XR_FUNC bool xi_value_type_is_result_group(const XiValue *v);
XR_FUNC bool xi_value_type_is_countdown_latch(const XiValue *v);
XR_FUNC bool xi_value_type_is_semaphore(const XiValue *v);
XR_FUNC bool xi_value_type_is_event_count(const XiValue *v);
XR_FUNC bool xi_value_type_is_unknown(const XiValue *v);

/* Resolve a direct or shared-slot import value to its canonical import
 * identity.  This is the IR-level source of truth used by ownership analysis
 * and backend lowering; callers must not recover module identity from names or
 * duplicate shared-slot scans. */
XR_FUNC const XiImportRef *xi_value_import_ref(const XiFunc *func, const XiValue *value);

/* Resolve a statically known namespace or source-instance method to its exact
 * Xi function. Coroutine analysis and ARC both need the same answer: if either
 * layer reconstructs only one spelling, suspendability and parameter ownership
 * can disagree for the same call. NULL means the method is not closed by the
 * current module graph; callers must stay conservative rather than use names
 * as authority. */
XR_FUNC XiFunc *xi_value_resolve_method_callee(const XiFunc *caller, const XiValue *call);

/* True when the module-graph import resolver has already run over this
 * reference and bound it to no source module, function, shared slot or export
 * slot: the sealed native ABI registry is then its only possible target.
 *
 * A reference the resolver never visited is NOT grounded, even when its
 * module/member spelling matches a native declaration - a source module may
 * still shadow that spelling once resolution runs.  The semantic plan draws
 * exactly this line (an unvisited reference classifies as unresolved and is
 * granted no call-target authority), so IR analyses that turn a native
 * identity into a proof must ask this question first. */
XR_FUNC bool xi_import_ref_is_grounded_native(const XiImportRef *ref);

/* True when the resolver bound this reference to a source module it can read. */
XR_FUNC bool xi_import_ref_is_source_module(const XiImportRef *ref);

/* True when this reference is grounded and names a module the sealed stdlib
 * definition registry declares: the registry is then its complete member set. */
XR_FUNC bool xi_import_ref_is_native_stdlib(const XiImportRef *ref);

/* True when a reference that does name a module resolves to neither a readable
 * source module nor a declared native one - because the module-graph resolver
 * never visited it, or visited it and bound nothing.
 *
 * The semantic plan classifies exactly this state as unresolved and grants a
 * call through such a reference no call-target authority at all, which in turn
 * forbids a coroutine state at that call.  An IR analysis must therefore not
 * read the reference as an open target set to be rejected: the plan has already
 * settled the call as identified and non-suspending, and answering otherwise
 * either fails a compile the plan accepts or creates a state it will reject. */
XR_FUNC bool xi_import_ref_is_unresolved(const XiImportRef *ref);

/* Numeric facts at a use site.
 * Uses existing range annotations when available and, if needed, dominating
 * branch guards on the path to 'site'. */
XR_FUNC bool xi_value_known_positive_at(const XiFunc *f, const XiValue *value, const XiBlock *site);
XR_FUNC bool xi_value_known_nonnegative_at(const XiFunc *f, const XiValue *value,
                                           const XiBlock *site);
XR_FUNC bool xi_value_known_ge_at(const XiFunc *f, const XiValue *value, const XiBlock *site,
                                  int64_t lower_bound);

/* The class declaration a call constructs, or NULL when the call is not a
 * source class construction.
 *
 * A construction is recognized from the lowering-proved constructor flag plus
 * the frozen module class row the callee's shared slot names, so the answer
 * never depends on how the callee value was spelled at the source level. On a
 * match, 'out_constructor' receives the declared instance constructor body, or
 * NULL when the class declares none and the construction is therefore a bare
 * allocation with no user code to run.
 *
 * Every layer that has to decide what a construction does - coroutine
 * resolution completeness and callsite suspendability alike - asks this one
 * query, so no two layers can disagree about which calls build an instance. */
XR_FUNC const struct XiClassData *xi_value_class_constructor_call(const XiFunc *func,
                                                                  const XiValue *call,
                                                                  const XiFunc **out_constructor);

#endif  // XI_VALUE_QUERY_H
