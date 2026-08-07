/* Canonical AOT callable descriptor shared by hosted and freestanding runtimes. */
#ifndef XRT_CALLABLE_H
#define XRT_CALLABLE_H

#include <stdint.h>

typedef struct XrAotCallableDesc {
    uint32_t target_id;
    uint32_t effect_bits;
    uint64_t signature_key;
    void (*sync_entry)(void); /* NULL when the verified target is suspendable-only. */
} XrAotCallableDesc;

#endif /* XRT_CALLABLE_H */
