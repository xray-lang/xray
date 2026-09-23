/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_channel_buffer.c - Independent FIFO model for channel buffer state
 */

#include "../test_framework.h"
#include "runtime/core/xr_channel_buffer.h"

typedef struct Message {
    uint64_t sequence;
    uint8_t bytes[13];
} Message;

TEST(typed_messages_match_linear_fifo) {
    for (uint32_t capacity = 0u; capacity <= 17u; ++capacity) {
        XrChannelBufferState state = {.capacity = capacity};
        Message physical[17] = {0}, expected[17] = {0};
        uint32_t count = 0u, random = 713u;
        for (uint32_t step = 0u; step < 10000u; ++step) {
            random = random * UINT32_C(1664525) + UINT32_C(1013904223);
            uint32_t slot = UINT32_MAX;
            if ((random >> 16u) & 1u) {
                bool accepted = xr_channel_buffer_push(&state, &slot);
                ASSERT_EQ_INT(accepted, count < capacity);
                if (accepted) {
                    ASSERT_LT(slot, capacity);
                    Message message = {.sequence = step};
                    for (uint32_t byte = 0u; byte < sizeof(message.bytes); ++byte)
                        message.bytes[byte] = (uint8_t) (step + byte);
                    expected[count++] = message;
                    physical[slot] = message;
                } else {
                    ASSERT_EQ_UINT(slot, UINT32_MAX);
                }
            } else {
                bool available = xr_channel_buffer_pop(&state, &slot);
                ASSERT_EQ_INT(available, count != 0u);
                if (available) {
                    ASSERT_LT(slot, capacity);
                    ASSERT_EQ_UINT(physical[slot].sequence, expected[0].sequence);
                    ASSERT_EQ_INT(memcmp(physical[slot].bytes, expected[0].bytes, 13u), 0);
                    --count;
                    memmove(expected, expected + 1, count * sizeof(*expected));
                } else {
                    ASSERT_EQ_UINT(slot, UINT32_MAX);
                }
            }
            ASSERT_TRUE(xr_channel_buffer_valid(&state));
            ASSERT_EQ_UINT(state.count, count);
            for (uint32_t index = 0u; index < count; ++index) {
                slot = xr_channel_buffer_slot(&state, index);
                ASSERT_LT(slot, capacity);
                ASSERT_EQ_UINT(physical[slot].sequence, expected[index].sequence);
            }
            ASSERT_EQ_UINT(xr_channel_buffer_slot(&state, count), UINT32_MAX);
        }
    }
}

TEST(maximum_capacity_does_not_overflow_indices) {
    const uint32_t capacities[] = {1u, 3u, UINT32_MAX / 2u, UINT32_MAX - 1u, UINT32_MAX};
    for (uint32_t sample = 0u; sample < sizeof(capacities) / sizeof(capacities[0]); ++sample) {
        uint32_t capacity = capacities[sample];
        uint32_t head = capacity - 1u;
        XrChannelBufferState state = {
            .capacity = capacity, .count = capacity, .read_index = head, .write_index = head,
        };
        ASSERT_TRUE(xr_channel_buffer_valid(&state));
        const uint32_t offsets[] = {0u, capacity / 2u, capacity - 1u};
        for (uint32_t index = 0u; index < 3u; ++index)
            ASSERT_EQ_UINT(xr_channel_buffer_slot(&state, offsets[index]),
                           ((uint64_t) head + offsets[index]) % capacity);
        uint32_t slot = UINT32_MAX;
        ASSERT_FALSE(xr_channel_buffer_push(&state, &slot));
        ASSERT_EQ_UINT(slot, UINT32_MAX);
        ASSERT_TRUE(xr_channel_buffer_pop(&state, &slot));
        ASSERT_EQ_UINT(slot, head);
        ASSERT_TRUE(xr_channel_buffer_push(&state, &slot));
        ASSERT_EQ_UINT(slot, head);
        ASSERT_TRUE(xr_channel_buffer_valid(&state));
        ASSERT_EQ_UINT(state.count, capacity);
    }
}

TEST(invalid_state_cannot_reserve_or_consume_storage) {
    const XrChannelBufferState invalid[] = {
        {.count = 1u}, {.write_index = 1u}, {.read_index = 1u},
        {.capacity = 1u, .count = 2u}, {.capacity = 2u, .read_index = 2u},
        {.capacity = 2u, .write_index = 2u},
        {.capacity = 3u, .count = 1u, .write_index = 2u},
    };
    for (uint32_t index = 0u; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        XrChannelBufferState state = invalid[index];
        uint32_t slot = 99u;
        ASSERT_FALSE(xr_channel_buffer_valid(&state));
        ASSERT_FALSE(xr_channel_buffer_push(&state, &slot));
        ASSERT_FALSE(xr_channel_buffer_pop(&state, &slot));
        ASSERT_EQ_UINT(slot, 99u);
        ASSERT_EQ_INT(memcmp(&state, &invalid[index], sizeof(state)), 0);
        ASSERT_EQ_UINT(xr_channel_buffer_slot(&state, 0u), UINT32_MAX);
    }
    XrChannelBufferState state = {.capacity = 1u};
    ASSERT_FALSE(xr_channel_buffer_push(&state, NULL));
    ASSERT_FALSE(xr_channel_buffer_pop(&state, NULL));
    ASSERT_FALSE(xr_channel_buffer_valid(NULL));
    ASSERT_EQ_UINT(state.count, 0u);
}

TEST_MAIN_BEGIN()
RUN_TEST(typed_messages_match_linear_fifo);
RUN_TEST(maximum_capacity_does_not_overflow_indices);
RUN_TEST(invalid_state_cannot_reserve_or_consume_storage);
TEST_MAIN_END()
