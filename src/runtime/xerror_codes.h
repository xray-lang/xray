/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xerror_codes.h - Single source of truth for all Exxxx error codes
 *
 * KEY CONCEPT:
 *   Every user-visible error code is defined here as a plain #define.
 *   XrErrorCode is typedef'd to int in xerror.h.
 *   Codes follow Exxxx format: E01xx Lexer, E02xx Syntax, E03xx Compile/Analysis,
 *   E04xx Runtime, E05xx Module, E06xx IO, E08xx Removed-syntax, E09xx Internal.
 */

#ifndef XERROR_CODES_H
#define XERROR_CODES_H

/* ---- Success ---- */
#define XR_OK 0

/* ---- Lexer errors (E01xx) ---- */
#define XR_ERR_LEX_INVALID_CHAR 101
#define XR_ERR_LEX_UNTERMINATED_STR 102
#define XR_ERR_LEX_INVALID_NUMBER 103
#define XR_ERR_LEX_INVALID_ESCAPE 104

/* ---- Syntax errors (E02xx) ---- */
#define XR_ERR_SYN_UNEXPECTED_TOKEN 201
#define XR_ERR_SYN_EXPECTED_EXPR 202
#define XR_ERR_SYN_EXPECTED_STMT 203
#define XR_ERR_SYN_UNCLOSED_PAREN 204
#define XR_ERR_SYN_UNCLOSED_BRACE 205
#define XR_ERR_SYN_UNCLOSED_BRACKET 206
#define XR_ERR_SYN_INVALID_ASSIGN 207
#define XR_ERR_SYN_EFFECTLESS_STMT 208

/* ---- Compile errors (E03xx, 301-319) ---- */
#define XR_ERR_CMP_UNDEFINED_VAR 301
#define XR_ERR_CMP_REDEFINED_VAR 302
#define XR_ERR_CMP_CONST_ASSIGN 303
#define XR_ERR_CMP_INVALID_BREAK 304
#define XR_ERR_CMP_INVALID_CONTINUE 305
#define XR_ERR_CMP_INVALID_RETURN 306
#define XR_ERR_CMP_TOO_MANY_PARAMS 307
#define XR_ERR_CMP_TOO_MANY_LOCALS 308
#define XR_ERR_CMP_TOO_MANY_CONSTANTS 309
#define XR_ERR_CMP_TOO_MANY_UPVALUES 310
#define XR_ERR_CMP_JUMP_TOO_LARGE 311

/* ---- Compile-time type errors (E03xx, 320-329) ---- */
#define XR_ERR_TYPE_NOT_CALLABLE 321
#define XR_ERR_TYPE_NOT_INDEXABLE 322
#define XR_ERR_TYPE_NOT_ITERABLE 323
#define XR_ERR_TYPE_INVALID_OPERAND 324

/* ---- Static analysis errors (E03xx, 350-399) ---- */
#define XR_ERR_ANALYZE 350
#define XR_ERR_ANALYZE_UNDEFINED_VAR 351
#define XR_ERR_ANALYZE_TYPE_MISMATCH 352
#define XR_ERR_ANALYZE_CONST_ASSIGN 353
#define XR_ERR_ANALYZE_NOT_CALLABLE 354
#define XR_ERR_ANALYZE_WRONG_ARG_COUNT 355
#define XR_ERR_ANALYZE_ARG_TYPE 356
#define XR_ERR_ANALYZE_GENERIC_COUNT 357
#define XR_ERR_ANALYZE_GENERIC_CONSTRAINT 358
#define XR_ERR_ANALYZE_SUPER_FIRST 359
#define XR_ERR_ANALYZE_SUPER_THIS 360
#define XR_ERR_ANALYZE_SUPER_REQUIRED 361
#define XR_ERR_ANALYZE_SUPER_INVALID 362
#define XR_ERR_ANALYZE_CLOSURE_CAPTURE 363
#define XR_ERR_ANALYZE_AWAIT_TYPE 364
#define XR_ERR_ANALYZE_MISSING_TYPE 365
#define XR_ERR_ANALYZE_INTERFACE_NOT_IMPLEMENTED 367
#define XR_ERR_ANALYZE_TUPLE_FIELD_NAME 368
#define XR_ERR_ANALYZE_TUPLE_FIELD_RANGE 369
#define XR_ERR_ANALYZE_THROW_NON_EXCEPTION 370
#define XR_ERR_ANALYZE_MATCH_NOT_EXHAUSTIVE 371
#define XR_ERR_ANALYZE_USED_BEFORE_ASSIGN 372
#define XR_ERR_ANALYZE_TUPLE_IMMUTABLE 373
#define XR_ERR_ANALYZE_OVERRIDE_MISMATCH 374
#define XR_ERR_ANALYZE_HASHABLE_CONTRACT 375
#define XR_ERR_ANALYZE_CONDITION_TYPE 376
#define XR_ERR_ANALYZE_VISIBILITY 377
#define XR_ERR_ANALYZE_CONST_FIELD 378
#define XR_ERR_ANALYZE_POSSIBLY_NULL 379
#define XR_ERR_ANALYZE_UNKNOWN_FIELD 380
#define XR_ERR_ANALYZE_MISSING_FIELD 381
/* Borrowed-view (Slice<T>, Ptr<T>/MutPtr<T>) lifetime violations. */
#define XR_ERR_ANALYZE_BORROW_CONFLICT 382
#define XR_ERR_ANALYZE_BORROW_ESCAPE 383
#define XR_ERR_ANALYZE_BORROW_SOURCE 384
/* A generator body reached, or could not be proven not to reach, the scheduler.
 * A generator frame is resumed by the iterator driving it; parking it on the
 * scheduler instead strands the driver and drops the remaining yields. */
#define XR_ERR_ANALYZE_GENERATOR_SUSPEND 385
/* A generator body registered a deferred action.  A generator abandoned before
 * exhaustion is never resumed, so the deferred action would silently not run --
 * an unrunnable cleanup in the language's only deterministic cleanup mechanism. */
#define XR_ERR_ANALYZE_GENERATOR_DEFER 386
#define XR_ERR_ANALYZE_DEFER_MAY_THROW 387
/* Monomorphization budgets.  Both are hard errors because the alternative --
 * leaving the call generic -- silently reintroduces boxing under a contract
 * that may have proven it absent. */
/* The whole-program instance budget was exhausted (breadth). */
#define XR_ERR_ANALYZE_MONO_BUDGET 388
/* A specialization chain exceeded the nesting budget (depth).  Reaching this
 * usually means a generic instantiates itself at an ever-larger type, which
 * has no finite monomorphization. */
#define XR_ERR_ANALYZE_MONO_DEPTH 389
#define XR_ERR_ANALYZE_UNION_INDISCRIMINABLE 390
/* An ownership root failed the uniqueness evidence `move` requires: a live
 * local alias, a root published into a heap graph, unknown provenance, or an
 * incomplete storage plan.  The message names which axis failed. */
#define XR_ERR_ANALYZE_MOVE_NOT_UNIQUE 391
/* A deferred action reached, or could not be proven not to reach, the
 * scheduler.  A defer body runs on a frame that is already leaving and has no
 * suspension point left to park at, so reaching the scheduler drops the rest
 * of the cleanup instead of resuming it. */
#define XR_ERR_ANALYZE_DEFER_SUSPEND 392
/* A closure captured a variable and was then stored into a field of that same
 * variable, so the object and the closure keep each other alive. Xray does not
 * reclaim reference cycles (spec 16.8), and `weak` is a field modifier that
 * cannot reach a capture edge, so this shape has no language mechanism to
 * break it — only the `defer` idiom. Reported at compile time because the
 * capture set and the assignment target are both dataflow facts here. */
#define XR_ERR_ANALYZE_CLOSURE_CYCLE 393
/* A `weak` field violated one of its rules:
 * W2 the declared type must be nullable — a weak slot reads null the instant
 * its target dies, so a non-nullable one would be a lie; W4 only an
 * EXEC_LOCAL-domain object may carry one, because clearing is driven by a
 * coroutine-local destroy hook that a shared object never runs. */
#define XR_ERR_ANALYZE_WEAK_FIELD 394
/* A cleanup body attempted to transfer control to its owning function or to a
 * loop outside the cleanup body. */
#define XR_ERR_ANALYZE_DEFER_CONTROL 395
/* Ownership binding-state axis: a binding read while MOVED / MAYBE_MOVED.
 * Control-flow facts, not type mismatches — kept apart from E0352 so the
 * diagnostic names the axis that actually failed. */
#define XR_ERR_ANALYZE_BINDING_STATE 396
/* Ownership capability/constraint axis: const or synchronization capability
 * as a move source, moves a loop backedge would repeat, module-static
 * consumption. Permissions, not argument-type errors (E0356). */
#define XR_ERR_ANALYZE_MOVE_CAPABILITY 397

/* ---- Runtime type errors (E04xx, 400-406) ---- */
#define XR_ERR_RUNTIME 400
#define XR_ERR_TYPE_NO_PROPERTY 401
#define XR_ERR_TYPE_NO_INDEX 402
#define XR_ERR_TYPE_NO_CALL 403
#define XR_ERR_TYPE_MISMATCH 404
#define XR_ERR_TYPE_NO_METHOD 405
#define XR_ERR_TYPE_NO_OPERATOR 406

/* ---- Runtime null errors (E04xx, 410-413) ---- */
#define XR_ERR_NULL_PROPERTY 410
#define XR_ERR_NULL_INDEX 411
#define XR_ERR_NULL_CALL 412
#define XR_ERR_NULL_UNWRAP 413

/* ---- Runtime arithmetic errors (E04xx, 420-422) ---- */
#define XR_ERR_DIV_BY_ZERO 420
#define XR_ERR_MOD_BY_ZERO 421
#define XR_ERR_OVERFLOW 422

/* ---- Runtime index/key errors (E04xx, 430-432) ---- */
#define XR_ERR_INDEX_OUT_OF_BOUNDS 430
#define XR_ERR_KEY_NOT_FOUND 431
/* An Iterator<T> was pulled past its end.  The pull protocol is two-step
 * (LANGUAGE_SPEC 5.3.6): hasNext() is the exhaustion test and next() returns T,
 * not T?, so there is no in-band value that can mean "no element left".
 * Reporting the violation is the only alternative to handing back a zero value
 * or a null the static type forbids. */
#define XR_ERR_ITERATOR_EXHAUSTED 432

/* ---- Runtime system errors (E04xx, 440-444) ---- */
#define XR_ERR_STACK_OVERFLOW 440
#define XR_ERR_OUT_OF_MEMORY 441
#define XR_ERR_MATCH_FAILURE 442
/* Error escaped a defer body past the E0387 static rule (FFI boundary).
 * Uncatchable: terminates the process. See spec §8.3.1 rule D3. */
#define XR_ERR_DEFER_THROW 443
/* A dynamically resolved cleanup call attempted to suspend or spawn. The
 * guard fires before scheduler or task state is created. */
#define XR_ERR_DEFER_ASYNC 444

/* ---- Runtime argument errors (E04xx, 450-451) ---- */
#define XR_ERR_WRONG_ARG_COUNT 450
#define XR_ERR_INVALID_ARG_TYPE 451

/* ---- Runtime coroutine errors (E04xx, 460-462) ---- */
#define XR_ERR_CORO_DEAD 460
#define XR_ERR_CORO_CANCELLED 461
#define XR_ERR_TASK_ALREADY_TAKEN 462

/* ---- Runtime stdlib errors (E04xx, 470-489) ---- */
#define XR_ERR_JSON_PARSE 470
#define XR_ERR_JSON_INVALID 471
#define XR_ERR_REGEX_COMPILE 475
#define XR_ERR_REGEX_PATTERN 476
#define XR_ERR_TLS_UNAVAILABLE 480

/* ---- Module errors (E05xx) ---- */
#define XR_ERR_MOD_NOT_FOUND 501
#define XR_ERR_MOD_LOAD_FAILED 502
#define XR_ERR_MOD_NO_EXPORT 503
#define XR_ERR_MOD_CIRCULAR 504

/* ---- IO errors (E06xx) ---- */
#define XR_ERR_IO_FILE_NOT_FOUND 601
#define XR_ERR_IO_READ_FAILED 602
#define XR_ERR_IO_WRITE_FAILED 603
#define XR_ERR_IO_PERMISSION_DENIED 604

/* ---- Coroutine-specific errors (E07xx) ---- */
#define XR_ERR_CORO_DEADLOCK 701
#define XR_ERR_CORO_CHANNEL_CLOSED 702
#define XR_ERR_CORO_LIMIT_EXCEEDED 703

/* ---- Removed-syntax errors (E08xx) ---- */
#define XR_ERR_SYN_RETURN_MULTI_REMOVED 801
#define XR_ERR_SYN_BINDING_CAPABILITY_REMOVED 802
#define XR_ERR_SYN_FOR_FLAT_REMOVED 803
#define XR_ERR_SYN_VOID_REMOVED 804
#define XR_ERR_SYN_PARAM_MODE_PREFIX_REMOVED 805
#define XR_ERR_SYN_PARAM_MOVE_MODE_REMOVED 806
#define XR_ERR_SYN_PARAM_MODE_COMBINED_REMOVED 807
#define XR_ERR_SYN_PARAM_MODE_POSTFIX_REMOVED 808
#define XR_ERR_SYN_CALL_IN_MARKER_REMOVED 809

/* ---- Internal errors (E09xx) ---- */
#define XR_ERR_INTERNAL 900
#define XR_ERR_NOT_IMPLEMENTED 901
#define XR_ERR_UNKNOWN 999

#endif  // XERROR_CODES_H
