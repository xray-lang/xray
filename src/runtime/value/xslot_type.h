/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xslot_type.h - Slot type tags for hybrid value system
 *
 * KEY CONCEPT:
 *   Describes what a stack slot or field contains.
 *   Used by stack map, upvalue descriptors, and field descriptors.
 *   NOT stored in the value itself - stored in metadata.
 *
 * WHY THIS DESIGN:
 *   - Known-type paths use raw values (no tag overhead), saving box/unbox cost
 *   - Unknown-type paths use tagged XrValue (16-byte struct)
 *   - GC uses stack map to distinguish pointers from raw values
 */

#ifndef XSLOT_TYPE_H
#define XSLOT_TYPE_H

#include <stdint.h>

typedef enum {
    XR_SLOT_ANY = 0,   // Tagged XrValue (untyped / dynamic code)
    XR_SLOT_I64 = 1,   // int64_t (all integer widths widen here at runtime)
    XR_SLOT_F64 = 2,   // double (F32 widens to F64 at runtime)
    XR_SLOT_PTR = 3,   // raw GC pointer (heap object)
    XR_SLOT_BOOL = 4,  // bool (stored as 0/1)
} XrSlotType;

// Is this slot type a GC-traceable reference?
#define XR_SLOT_IS_GC(st) ((st) == XR_SLOT_PTR || (st) == XR_SLOT_ANY)

// Is this slot type an integer?
#define XR_SLOT_IS_INT(st) ((st) == XR_SLOT_I64)

// Is this slot type a float?
#define XR_SLOT_IS_FLOAT(st) ((st) == XR_SLOT_F64)

// Is this slot type a numeric primitive (no GC, no pointer)?
#define XR_SLOT_IS_NUMERIC(st) ((st) == XR_SLOT_I64 || (st) == XR_SLOT_F64)

#endif  // XSLOT_TYPE_H
