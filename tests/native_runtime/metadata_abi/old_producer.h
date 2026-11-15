/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XR_NATIVE_OLD_PRODUCER_H
#define XR_NATIVE_OLD_PRODUCER_H
#include <stdbool.h>
#include <stdint.h>
/* This TU exports only opaque C values; its actual calls compile with the
 * frozen pre-cutover public headers, independently of the new consumer. */
enum {
    OLD_SCOPE_ENTER, OLD_CAPTURE, OLD_ENTRY, OLD_CELL_READ, OLD_CELL_WRITE,
    OLD_CALLABLE_STATE, OLD_BOXED_NEW, OLD_BOXED_STATE, OLD_ACTION_COUNT
};
enum { OLD_SCOPE_NEW, OLD_SCOPE_COPY, OLD_SCOPE_BIND, OLD_SCOPE_LEAVE, OLD_CELL_NEW, OLD_CELL_MAP_NEW, OLD_CELL_BIND, OLD_SCOPE_ACTION_COUNT };
void old_metadata_scope_invoke(uint32_t action, bool poison, void *scope);
const void *old_metadata_image(bool poison);
void old_metadata_invoke(uint32_t action, bool poison, void *state);
uint32_t old_metadata_view(const void *layout, bool *output_untouched);
uint32_t old_metadata_layout(bool poison, bool *output_untouched);
#endif
