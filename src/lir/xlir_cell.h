/* Copyright (c) 2026 Xinglei Xu. MIT License. */
#ifndef XLIR_CELL_H
#define XLIR_CELL_H
#include "xlir_types.h"

/* Owned Cell identities are separate from synchronous activation capabilities.
 * Every alias of a promoted binding names this same exact payload owner. */
typedef struct XrLirCellType { XrLirType payload, array_element; } XrLirCellType;
typedef enum XrLirCellTransport {
    XR_LIR_CELL_TRANSPORT_NONE, XR_LIR_CELL_TRANSPORT_BORROW, XR_LIR_CELL_TRANSPORT_OWNED
} XrLirCellTransport;
typedef enum XrLirCellOperation {
    XR_LIR_CELL_OP_NONE, XR_LIR_CELL_OP_NEW, XR_LIR_CELL_OP_READ,
    XR_LIR_CELL_OP_WRITE, XR_LIR_CELL_OP_BORROW_MAP,
    XR_LIR_CELL_OP_COPY, XR_LIR_CELL_OP_BIND, XR_LIR_CELL_OP_KEEP
} XrLirCellOperation;
XR_FUNC XrLirStatus xr_lir_cell_type(XrLirModule *, XrLirCellType, XrLirType *output);
XR_FUNC XrLirStatus xr_lir_cell_type_view(const XrLirModule *, XrLirType, XrLirCellType *output);
/* The admitted identity-value subset has an exact I64 or Map payload. This
 * classifies a descriptor only; it grants no live owner or payload access. */
XR_FUNC bool xr_lir_cell_value_type(const XrLirModule *, XrLirType, XrLirCellType *optional);
#endif // XLIR_CELL_H
