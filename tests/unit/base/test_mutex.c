/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_mutex.c - Unit tests for cross-platform three-state mutex
 *
 * KEY CONCEPT:
 *   Tests XrAdaptiveMutex operations: init, lock, unlock, trylock.
 *   Single-threaded tests verify basic semantics and state transitions.
 */

#include "../test_framework.h"
#include "base/xmutex.h"

/* ========== Basic Operations ========== */

TEST(mutex_init) {
    XrAdaptiveMutex m;
    xr_amutex_init(&m);
    ASSERT_EQ_INT(atomic_load(&m.state), XR_AMUTEX_UNLOCKED);
}

TEST(mutex_lock_unlock) {
    XrAdaptiveMutex m;
    xr_amutex_init(&m);

    xr_amutex_lock(&m);
    ASSERT_NE(atomic_load(&m.state), XR_AMUTEX_UNLOCKED);

    xr_amutex_unlock(&m);
    ASSERT_EQ_INT(atomic_load(&m.state), XR_AMUTEX_UNLOCKED);
}

TEST(mutex_lock_unlock_repeated) {
    XrAdaptiveMutex m;
    xr_amutex_init(&m);

    for (int i = 0; i < 100; i++) {
        xr_amutex_lock(&m);
        xr_amutex_unlock(&m);
    }
    ASSERT_EQ_INT(atomic_load(&m.state), XR_AMUTEX_UNLOCKED);
}

/* ========== Trylock ========== */

TEST(mutex_trylock_success) {
    XrAdaptiveMutex m;
    xr_amutex_init(&m);

    ASSERT_TRUE(xr_amutex_trylock(&m));
    ASSERT_NE(atomic_load(&m.state), XR_AMUTEX_UNLOCKED);

    xr_amutex_unlock(&m);
    ASSERT_EQ_INT(atomic_load(&m.state), XR_AMUTEX_UNLOCKED);
}

TEST(mutex_trylock_fail) {
    XrAdaptiveMutex m;
    xr_amutex_init(&m);

    xr_amutex_lock(&m);
    // Already locked, trylock should fail
    ASSERT_FALSE(xr_amutex_trylock(&m));

    xr_amutex_unlock(&m);
    // Now trylock should succeed
    ASSERT_TRUE(xr_amutex_trylock(&m));
    xr_amutex_unlock(&m);
}

/* ========== State Transitions ========== */

TEST(mutex_state_after_lock) {
    XrAdaptiveMutex m;
    xr_amutex_init(&m);

    // After lock, state should be LOCKED (not SLEEPING in single-thread)
    xr_amutex_lock(&m);
    ASSERT_EQ_INT(atomic_load(&m.state), XR_AMUTEX_LOCKED);
    xr_amutex_unlock(&m);
}

TEST(mutex_state_after_unlock) {
    XrAdaptiveMutex m;
    xr_amutex_init(&m);

    xr_amutex_lock(&m);
    xr_amutex_unlock(&m);
    ASSERT_EQ_INT(atomic_load(&m.state), XR_AMUTEX_UNLOCKED);
}

/* ========== Main ========== */

TEST_MAIN_BEGIN()

RUN_TEST_SUITE("Mutex - Basic Operations");
RUN_TEST(mutex_init);
RUN_TEST(mutex_lock_unlock);
RUN_TEST(mutex_lock_unlock_repeated);

RUN_TEST_SUITE("Mutex - Trylock");
RUN_TEST(mutex_trylock_success);
RUN_TEST(mutex_trylock_fail);

RUN_TEST_SUITE("Mutex - State Transitions");
RUN_TEST(mutex_state_after_lock);
RUN_TEST(mutex_state_after_unlock);

TEST_MAIN_END()
