/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_channel_storage.c - Owned message transfer and final release races
 */

#include "../test_framework.h"
#include "base/xmalloc.h"
#include "os/os_thread.h"
#include "runtime/core/xr_channel_storage.h"

typedef struct OwnedMessage {
    _Atomic(uint32_t) *drops;
    uint32_t sequence;
    char text[16];
} OwnedMessage;

static void drop_message(void *opaque) {
    OwnedMessage *message = opaque;
    atomic_fetch_add_explicit(message->drops, 1u, memory_order_relaxed);
    xr_free(message);
}

static XrChannelMessageOwner new_message(_Atomic(uint32_t) *drops, uint32_t sequence) {
    OwnedMessage *message = xr_malloc(sizeof(*message));
    if (!message)
        return (XrChannelMessageOwner) {0};
    message->drops = drops;
    message->sequence = sequence;
    memcpy(message->text, "owned message", sizeof("owned message"));
    return (XrChannelMessageOwner) {.value = message, .drop = drop_message};
}

TEST(send_commits_ownership_only_when_accepted) {
    _Atomic(uint32_t) drops = 0u;
    XrChannelStorage storage;
    XrChannelMessageOwner slots[1];
    ASSERT_TRUE(xr_channel_storage_init(&storage, slots, 1u));
    XrChannelMessageOwner first = new_message(&drops, 11u);
    XrChannelMessageOwner second = new_message(&drops, 12u);
    ASSERT_NOT_NULL(first.value);
    ASSERT_NOT_NULL(second.value);
    void *identity = first.value;
    ASSERT_EQ_INT(xr_channel_storage_send(&storage, &first), XR_CHANNEL_STORAGE_OK);
    ASSERT_NULL(first.value);
    ASSERT_NULL(first.drop);
    void *unsent = second.value;
    ASSERT_EQ_INT(xr_channel_storage_send(&storage, &second), XR_CHANNEL_STORAGE_WOULD_BLOCK);
    ASSERT_TRUE(second.value == unsent);
    ASSERT_TRUE(xr_channel_storage_close(&storage));
    ASSERT_FALSE(xr_channel_storage_close(&storage));
    ASSERT_EQ_INT(xr_channel_storage_send(&storage, &second), XR_CHANNEL_STORAGE_CLOSED);
    ASSERT_TRUE(second.value == unsent);
    XrChannelMessageOwner received = {0};
    ASSERT_EQ_INT(xr_channel_storage_receive(&storage, &received), XR_CHANNEL_STORAGE_OK);
    ASSERT_TRUE(received.value == identity);
    ASSERT_EQ_UINT(((OwnedMessage *) received.value)->sequence, 11u);
    ASSERT_STR_EQ(((OwnedMessage *) received.value)->text, "owned message");
    received.drop(received.value);
    received = (XrChannelMessageOwner) {0};
    ASSERT_EQ_INT(xr_channel_storage_receive(&storage, &received), XR_CHANNEL_STORAGE_CLOSED);
    second.drop(second.value);
    ASSERT_EQ_INT(xr_channel_storage_release(&storage), XR_CHANNEL_STORAGE_LAST_OWNER);
    ASSERT_EQ_UINT(atomic_load(&drops), 2u);
    ASSERT_FALSE(xr_channel_storage_retain(&storage));
    ASSERT_EQ_INT(xr_channel_storage_release(&storage), XR_CHANNEL_STORAGE_INVALID);
}

TEST(final_owner_drops_only_queued_messages) {
    _Atomic(uint32_t) drops = 0u;
    XrChannelStorage storage;
    XrChannelMessageOwner slots[3];
    ASSERT_TRUE(xr_channel_storage_init(&storage, slots, 3u));
    ASSERT_TRUE(xr_channel_storage_retain(&storage));
    for (uint32_t index = 0u; index < 19u; ++index) {
        XrChannelMessageOwner message = new_message(&drops, index);
        ASSERT_NOT_NULL(message.value);
        ASSERT_EQ_INT(xr_channel_storage_send(&storage, &message), XR_CHANNEL_STORAGE_OK);
        if (index < 17u) {
            ASSERT_EQ_INT(xr_channel_storage_receive(&storage, &message), XR_CHANNEL_STORAGE_OK);
            ASSERT_EQ_UINT(((OwnedMessage *) message.value)->sequence, index);
            message.drop(message.value);
        }
    }
    ASSERT_EQ_UINT(atomic_load(&drops), 17u);
    ASSERT_EQ_INT(xr_channel_storage_release(&storage), XR_CHANNEL_STORAGE_OK);
    ASSERT_EQ_UINT(atomic_load(&drops), 17u);
    ASSERT_EQ_INT(xr_channel_storage_release(&storage), XR_CHANNEL_STORAGE_LAST_OWNER);
    ASSERT_EQ_UINT(atomic_load(&drops), 19u);
    ASSERT_EQ_UINT(storage.buffer.count, 0u);
    for (uint32_t index = 0u; index < 3u; ++index) {
        ASSERT_NULL(slots[index].value);
        ASSERT_NULL(slots[index].drop);
    }
}

typedef struct ReleaseRace {
    XrChannelStorage *storage;
    _Atomic(uint32_t) ready;
    _Atomic(bool) start;
    _Atomic(uint32_t) last;
    _Atomic(uint32_t) invalid;
} ReleaseRace;

static void *release_worker(void *opaque) {
    ReleaseRace *race = opaque;
    atomic_fetch_add_explicit(&race->ready, 1u, memory_order_release);
    while (!atomic_load_explicit(&race->start, memory_order_acquire)) {}
    XrChannelStorageResult result = xr_channel_storage_release(race->storage);
    if (result == XR_CHANNEL_STORAGE_LAST_OWNER)
        atomic_fetch_add_explicit(&race->last, 1u, memory_order_relaxed);
    else if (result != XR_CHANNEL_STORAGE_OK)
        atomic_fetch_add_explicit(&race->invalid, 1u, memory_order_relaxed);
    return NULL;
}

TEST(concurrent_last_release_has_one_cleanup_owner) {
    for (uint32_t iteration = 0u; iteration < 100u; ++iteration) {
        _Atomic(uint32_t) drops = 0u;
        XrChannelStorage storage;
        XrChannelMessageOwner slots[1];
        ASSERT_TRUE(xr_channel_storage_init(&storage, slots, 1u));
        ASSERT_TRUE(xr_channel_storage_retain(&storage));
        XrChannelMessageOwner message = new_message(&drops, iteration);
        ASSERT_NOT_NULL(message.value);
        ASSERT_EQ_INT(xr_channel_storage_send(&storage, &message), XR_CHANNEL_STORAGE_OK);
        ReleaseRace race = {.storage = &storage};
        xr_thread_t first, second;
        ASSERT_TRUE(xr_thread_create(&first, release_worker, &race));
        bool created = xr_thread_create(&second, release_worker, &race);
        if (created)
            while (atomic_load_explicit(&race.ready, memory_order_acquire) != 2u) {}
        atomic_store_explicit(&race.start, true, memory_order_release);
        ASSERT_EQ_INT(xr_thread_join(first, NULL), 0);
        if (!created)
            (void) xr_channel_storage_release(&storage);
        ASSERT_TRUE(created);
        ASSERT_EQ_INT(xr_thread_join(second, NULL), 0);
        ASSERT_EQ_UINT(atomic_load(&race.last), 1u);
        ASSERT_EQ_UINT(atomic_load(&race.invalid), 0u);
        ASSERT_EQ_UINT(atomic_load(&drops), 1u);
    }
}

TEST(zero_capacity_and_invalid_calls_preserve_owners) {
    XrChannelStorage storage;
    ASSERT_FALSE(xr_channel_storage_init(NULL, NULL, 0u));
    ASSERT_FALSE(xr_channel_storage_init(&storage, NULL, 1u));
    ASSERT_TRUE(xr_channel_storage_init(&storage, NULL, 0u));
    _Atomic(uint32_t) drops = 0u;
    XrChannelMessageOwner message = new_message(&drops, 0u), empty = {0};
    ASSERT_NOT_NULL(message.value);
    ASSERT_EQ_INT(xr_channel_storage_send(&storage, &message), XR_CHANNEL_STORAGE_WOULD_BLOCK);
    ASSERT_EQ_INT(xr_channel_storage_receive(&storage, &message), XR_CHANNEL_STORAGE_INVALID);
    ASSERT_EQ_INT(xr_channel_storage_receive(&storage, &empty), XR_CHANNEL_STORAGE_WOULD_BLOCK);
    atomic_store(&storage.owners, UINT32_MAX);
    ASSERT_FALSE(xr_channel_storage_retain(&storage));
    ASSERT_EQ_UINT(atomic_load(&storage.owners), UINT32_MAX);
    atomic_store(&storage.owners, 1u);
    ASSERT_EQ_INT(xr_channel_storage_release(&storage), XR_CHANNEL_STORAGE_LAST_OWNER);
    ASSERT_EQ_UINT(atomic_load(&drops), 0u);
    message.drop(message.value);
    ASSERT_EQ_UINT(atomic_load(&drops), 1u);
}

TEST_MAIN_BEGIN()
RUN_TEST(send_commits_ownership_only_when_accepted);
RUN_TEST(final_owner_drops_only_queued_messages);
RUN_TEST(concurrent_last_release_has_one_cleanup_owner);
RUN_TEST(zero_capacity_and_invalid_calls_preserve_owners);
TEST_MAIN_END()
