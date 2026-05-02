/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xm_printer.h - Xm text dump for debugging
 */

#ifndef XM_PRINTER_H
#define XM_PRINTER_H

#include "xm.h"
#include <stdio.h>
#include "../base/xdefs.h"

// Print entire function IR to file stream
XR_FUNC void xm_print_func(FILE *out, XmFunc *func);

// Print a single basic block
XR_FUNC void xm_print_block(FILE *out, XmFunc *func, XmBlock *blk);

// Print a single instruction
XR_FUNC void xm_print_ins(FILE *out, XmFunc *func, XmIns *ins);

// Print a reference
XR_FUNC void xm_print_ref(FILE *out, XmFunc *func, XmRef ref);

#endif  // XM_PRINTER_H
