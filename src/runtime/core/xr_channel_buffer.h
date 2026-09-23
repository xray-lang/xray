/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xr_channel_buffer.h - Value-independent bounded channel FIFO state
 *
 * KEY CONCEPT:
 *   The caller holds the channel lock across index reservation and payload
 *   transfer. This kernel owns FIFO order, not payload layout, lifetime,
 *   allocation, close state, waiter selection or wake publication.
 */

#ifndef XR_CHANNEL_BUFFER_H
#define XR_CHANNEL_BUFFER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct XrChannelBufferState {
    uint32_t capacity;
    uint32_t count;
    uint32_t write_index;
    uint32_t read_index;
} XrChannelBufferState;

static inline bool xr_channel_buffer_valid(const XrChannelBufferState *state) {
    if (!state || state->count > state->capacity)
        return false;
    if (!state->capacity)
        return state->write_index == 0u && state->read_index == 0u;
    if (state->write_index >= state->capacity || state->read_index >= state->capacity)
        return false;
    uint32_t until_wrap = state->capacity - state->read_index;
    uint32_t write_index = state->count >= until_wrap
                               ? state->count - until_wrap : state->read_index + state->count;
    return state->write_index == write_index;
}

/* An offset is a logical occupied position, not a physical array index.
 * Subtract before adding so a valid ring near UINT32_MAX cannot wrap early. */
static inline uint32_t xr_channel_buffer_slot(const XrChannelBufferState *state, uint32_t offset) {
    if (!xr_channel_buffer_valid(state) || offset >= state->count)
        return UINT32_MAX;
    uint32_t until_wrap = state->capacity - state->read_index;
    return offset >= until_wrap ? offset - until_wrap : state->read_index + offset;
}

static inline bool xr_channel_buffer_push(XrChannelBufferState *state, uint32_t *slot) {
    if (!slot || !xr_channel_buffer_valid(state) || state->count == state->capacity)
        return false;
    *slot = state->write_index;
    state->write_index = state->write_index == state->capacity - 1u
                             ? 0u : state->write_index + 1u;
    ++state->count;
    return true;
}

static inline bool xr_channel_buffer_pop(XrChannelBufferState *state, uint32_t *slot) {
    if (!slot || !xr_channel_buffer_valid(state) || state->count == 0u)
        return false;
    *slot = state->read_index;
    state->read_index = state->read_index == state->capacity - 1u
                            ? 0u : state->read_index + 1u;
    --state->count;
    return true;
}

#endif // XR_CHANNEL_BUFFER_H
