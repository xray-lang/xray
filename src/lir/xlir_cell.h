/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLIR_CELL_H
#define XLIR_CELL_H
#include "xlir_types.h"

/* Owned Cell identities are separate from synchronous activation capabilities.
 * Every alias of a promoted binding names this same exact payload owner. */
typedef struct XrLirCellType { XrLirType payload, array_element; } XrLirCellType;
XR_FUNC XrLirStatus xr_lir_cell_type(XrLirModule *, XrLirCellType, XrLirType *output);
XR_FUNC XrLirStatus xr_lir_cell_type_view(const XrLirModule *, XrLirType, XrLirCellType *output);
#endif // XLIR_CELL_H
