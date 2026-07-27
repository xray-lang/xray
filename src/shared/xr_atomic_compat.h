/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_atomic_compat.h - C11 atomic spelling with a GNU/Clang C++ host bridge.
 *
 * AOT-generated translation units are C by default, but some governed ports
 * deliberately compile the generated core as C++.  std::atomic is not used for
 * that lane: its object representation is not a C ABI contract and its deleted
 * copy operations would change the generated POD model.  GNU/Clang __atomic
 * builtins operate on the same scalar storage used by C11 atomics, preserving
 * the public layout while retaining the requested memory ordering.
 */

#ifndef XR_ATOMIC_COMPAT_H
#define XR_ATOMIC_COMPAT_H

#include <stddef.h>
#include <stdint.h>

#if defined(__cplusplus)

#if !defined(__GNUC__) && !defined(__clang__)
#error "C++ AOT atomic compatibility requires GNU or Clang __atomic builtins"
#endif

/* C11 language spellings used by the standalone AOT headers and generated C.
 * Keep _Atomic function-style so ordinary C remains a standard C11 program. */
#define _Atomic(T) T
#define _Alignof(T) alignof(T)
#define _Static_assert static_assert
#define _Thread_local thread_local
#define _Noreturn __attribute__((noreturn))
#define __auto_type auto

typedef int memory_order;
#define memory_order_relaxed __ATOMIC_RELAXED
#define memory_order_consume __ATOMIC_CONSUME
#define memory_order_acquire __ATOMIC_ACQUIRE
#define memory_order_release __ATOMIC_RELEASE
#define memory_order_acq_rel __ATOMIC_ACQ_REL
#define memory_order_seq_cst __ATOMIC_SEQ_CST

typedef bool atomic_bool;
typedef char atomic_char;
typedef signed char atomic_schar;
typedef unsigned char atomic_uchar;
typedef short atomic_short;
typedef unsigned short atomic_ushort;
typedef int atomic_int;
typedef unsigned int atomic_uint;
typedef long atomic_long;
typedef unsigned long atomic_ulong;
typedef long long atomic_llong;
typedef unsigned long long atomic_ullong;
typedef intptr_t atomic_intptr_t;
typedef uintptr_t atomic_uintptr_t;
typedef size_t atomic_size_t;
typedef ptrdiff_t atomic_ptrdiff_t;
typedef intmax_t atomic_intmax_t;
typedef uintmax_t atomic_uintmax_t;

/* GCC and Clang implement atomic_flag with one byte of scalar storage. */
typedef bool atomic_flag;
#define ATOMIC_FLAG_INIT false
#define ATOMIC_VAR_INIT(value) (value)

#define atomic_init(object, desired) __atomic_store_n((object), (desired), __ATOMIC_RELAXED)
#define atomic_is_lock_free(object) __atomic_is_lock_free(sizeof(*(object)), (object))

#define atomic_load_explicit(object, order) __atomic_load_n((object), (order))
#define atomic_store_explicit(object, desired, order) \
    __atomic_store_n((object), (desired), (order))
#define atomic_exchange_explicit(object, desired, order) \
    __atomic_exchange_n((object), (desired), (order))
#define atomic_compare_exchange_weak_explicit(object, expected, desired, success, failure) \
    __atomic_compare_exchange_n((object), (expected), (desired), true, (success), (failure))
#define atomic_compare_exchange_strong_explicit(object, expected, desired, success, failure) \
    __atomic_compare_exchange_n((object), (expected), (desired), false, (success), (failure))
#define atomic_fetch_add_explicit(object, operand, order) \
    __atomic_fetch_add((object), (operand), (order))
#define atomic_fetch_sub_explicit(object, operand, order) \
    __atomic_fetch_sub((object), (operand), (order))
#define atomic_fetch_and_explicit(object, operand, order) \
    __atomic_fetch_and((object), (operand), (order))
#define atomic_fetch_or_explicit(object, operand, order) \
    __atomic_fetch_or((object), (operand), (order))
#define atomic_fetch_xor_explicit(object, operand, order) \
    __atomic_fetch_xor((object), (operand), (order))

#define atomic_load(object) atomic_load_explicit((object), memory_order_seq_cst)
#define atomic_store(object, desired) \
    atomic_store_explicit((object), (desired), memory_order_seq_cst)
#define atomic_exchange(object, desired) \
    atomic_exchange_explicit((object), (desired), memory_order_seq_cst)
#define atomic_compare_exchange_weak(object, expected, desired) \
    atomic_compare_exchange_weak_explicit((object), (expected), (desired), memory_order_seq_cst, \
                                          memory_order_seq_cst)
#define atomic_compare_exchange_strong(object, expected, desired) \
    atomic_compare_exchange_strong_explicit((object), (expected), (desired), memory_order_seq_cst, \
                                            memory_order_seq_cst)
#define atomic_fetch_add(object, operand) \
    atomic_fetch_add_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_sub(object, operand) \
    atomic_fetch_sub_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_and(object, operand) \
    atomic_fetch_and_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_or(object, operand) \
    atomic_fetch_or_explicit((object), (operand), memory_order_seq_cst)
#define atomic_fetch_xor(object, operand) \
    atomic_fetch_xor_explicit((object), (operand), memory_order_seq_cst)

#define atomic_flag_test_and_set_explicit(object, order) __atomic_test_and_set((object), (order))
#define atomic_flag_clear_explicit(object, order) __atomic_clear((object), (order))
#define atomic_flag_test_and_set(object) \
    atomic_flag_test_and_set_explicit((object), memory_order_seq_cst)
#define atomic_flag_clear(object) atomic_flag_clear_explicit((object), memory_order_seq_cst)
#define atomic_thread_fence(order) __atomic_thread_fence((order))
#define atomic_signal_fence(order) __atomic_signal_fence((order))

#else

#include <stdatomic.h>

#endif /* __cplusplus */

#endif /* XR_ATOMIC_COMPAT_H */
