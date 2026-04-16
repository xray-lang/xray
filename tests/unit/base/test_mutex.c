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
 *   Tests XrMutex operations: init, lock, unlock, trylock.
 *   Single-threaded tests verify basic semantics and state transitions.
 */

#include "../test_framework.h"
#include "base/xmutex.h"

/* ========== Basic Operations ========== */

TEST(mutex_init) {
    XrMutex m;
    xr_mutex_init(&m);
    ASSERT_EQ_INT(atomic_load(&m.state), XR_MUTEX_UNLOCKED);
}

TEST(mutex_lock_unlock) {
    XrMutex m;
    xr_mutex_init(&m);

    xr_mutex_lock(&m);
    ASSERT_NE(atomic_load(&m.state), XR_MUTEX_UNLOCKED);

    xr_mutex_unlock(&m);
    ASSERT_EQ_INT(atomic_load(&m.state), XR_MUTEX_UNLOCKED);
}

TEST(mutex_lock_unlock_repeated) {
    XrMutex m;
    xr_mutex_init(&m);

    for (int i = 0; i < 100; i++) {
        xr_mutex_lock(&m);
        xr_mutex_unlock(&m);
    }
    ASSERT_EQ_INT(atomic_load(&m.state), XR_MUTEX_UNLOCKED);
}

/* ========== Trylock ========== */

TEST(mutex_trylock_success) {
    XrMutex m;
    xr_mutex_init(&m);

    ASSERT_TRUE(xr_mutex_trylock(&m));
    ASSERT_NE(atomic_load(&m.state), XR_MUTEX_UNLOCKED);

    xr_mutex_unlock(&m);
    ASSERT_EQ_INT(atomic_load(&m.state), XR_MUTEX_UNLOCKED);
}

TEST(mutex_trylock_fail) {
    XrMutex m;
    xr_mutex_init(&m);

    xr_mutex_lock(&m);
    // Already locked, trylock should fail
    ASSERT_FALSE(xr_mutex_trylock(&m));

    xr_mutex_unlock(&m);
    // Now trylock should succeed
    ASSERT_TRUE(xr_mutex_trylock(&m));
    xr_mutex_unlock(&m);
}

/* ========== State Transitions ========== */

TEST(mutex_state_after_lock) {
    XrMutex m;
    xr_mutex_init(&m);

    // After lock, state should be LOCKED (not SLEEPING in single-thread)
    xr_mutex_lock(&m);
    ASSERT_EQ_INT(atomic_load(&m.state), XR_MUTEX_LOCKED);
    xr_mutex_unlock(&m);
}

TEST(mutex_state_after_unlock) {
    XrMutex m;
    xr_mutex_init(&m);

    xr_mutex_lock(&m);
    xr_mutex_unlock(&m);
    ASSERT_EQ_INT(atomic_load(&m.state), XR_MUTEX_UNLOCKED);
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
