/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_intrinsic_flags.h - Effect flags and return-rep constants for
 *                        xi_intrinsic.def entries.
 *
 * Must be included before xi_intrinsic.def when the consumer needs
 * to read the effect / return-rep columns.  Consumers that only
 * need name/id/arity/helper can ignore these and define their
 * XI_INTRINSIC macro to discard the extra parameters.
 */

#ifndef XI_INTRINSIC_FLAGS_H
#define XI_INTRINSIC_FLAGS_H

/* ========== Intrinsic effect flags (bitfield) ========== */

#define IEFF_PURE 0x00 /* no observable side effects           */
#define IEFF_R 0x01    /* reads heap / shared state            */
#define IEFF_W 0x02    /* writes heap / shared state           */
#define IEFF_T 0x04    /* can throw or trap                    */
#define IEFF_IO 0x08   /* performs I/O (print, file, network)  */
#define IEFF_A 0x10    /* allocates on the managed heap        */

/* ========== Intrinsic return-value representation ========== */

#define IREP_VAL 0  /* returns a tagged XrValue               */
#define IREP_VOID 1 /* no meaningful return (side-effect only) */
#define IREP_I64 2  /* returns a raw int64                     */

#endif  // XI_INTRINSIC_FLAGS_H
