/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xir_printer.h - XIR text dump for debugging
 */

#ifndef XIR_PRINTER_H
#define XIR_PRINTER_H

#include "xir.h"
#include <stdio.h>
#include "../base/xdefs.h"

// Print entire function IR to file stream
XR_FUNC void xir_print_func(FILE *out, XirFunc *func);

// Print a single basic block
XR_FUNC void xir_print_block(FILE *out, XirFunc *func, XirBlock *blk);

// Print a single instruction
XR_FUNC void xir_print_ins(FILE *out, XirFunc *func, XirIns *ins);

// Print a reference
XR_FUNC void xir_print_ref(FILE *out, XirFunc *func, XirRef ref);

#endif  // XIR_PRINTER_H
