/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaddressability.h - Canonical analyzer-side lvalue address classification.
 */

#ifndef XADDRESSABILITY_H
#define XADDRESSABILITY_H

#include "../../base/xdefs.h"
#include "../../base/xstorage.h"
#include <stdbool.h>
#include <stdint.h>

struct AstNode;
struct XaInferContext;
struct XaSymbol;
struct XrType;

typedef enum XaAddressKind {
    XA_ADDRESS_NONE = 0,
    XA_ADDRESS_MODULE_STATIC,
    XA_ADDRESS_STACK_LOCAL,
    XA_ADDRESS_PARAMETER,
    XA_ADDRESS_FIELD,
    XA_ADDRESS_FIXED_ARRAY_ELEMENT,
    XA_ADDRESS_OWNER_ELEMENT,
} XaAddressKind;

typedef enum XaAddressLifetime {
    XA_ADDRESS_LIFETIME_NONE = 0,
    XA_ADDRESS_LIFETIME_MODULE,
    XA_ADDRESS_LIFETIME_CALL,
    XA_ADDRESS_LIFETIME_LEXICAL,
    XA_ADDRESS_LIFETIME_OWNER,
} XaAddressLifetime;

typedef enum XaAddressRejectReason {
    XA_ADDRESS_OK = 0,
    XA_ADDRESS_REJECT_NOT_LVALUE,
    XA_ADDRESS_REJECT_TEMPORARY,
    XA_ADDRESS_REJECT_NO_NATIVE_LAYOUT,
    XA_ADDRESS_REJECT_STORAGE_MAY_MOVE,
    XA_ADDRESS_REJECT_READONLY,
    XA_ADDRESS_REJECT_DYNAMIC_OWNER_ELEMENT,
    XA_ADDRESS_REJECT_ESCAPE_UNPROVEN,
} XaAddressRejectReason;

typedef struct XaAddressability {
    XaAddressKind kind;
    XaAddressLifetime lifetime;
    XaAddressRejectReason rejection;
    struct XrType *pointee_type;
    struct XaSymbol *base_symbol;
    uint32_t field_offset;
    uint8_t storage_domain;   /* XrSemanticStorageDomain */
    uint8_t address_identity; /* XrAddressIdentity */
    bool native_layout_ok;
    bool mutable_ok;
    bool is_imported;
    bool is_shared;
} XaAddressability;

typedef struct XaPointerProvenance {
    XrAddressProvenance address;
    struct XaSymbol *owner_symbol;
    bool mixed;
} XaPointerProvenance;

/* Classifies every lvalue shape without opening a public address-taking
 * surface. A classified local/parameter remains rejected until the VM native
 * slot contract can prove a stable call-bound lifetime. */
XR_FUNC XaAddressability xa_classify_addressability(struct XaInferContext *ctx,
                                                    struct AstNode *expr, bool wants_mutable);
XR_FUNC bool xa_pointer_provenance_for_expr(struct XaInferContext *ctx, struct AstNode *expr,
                                            XaPointerProvenance *out);
XR_FUNC void xa_record_pointer_provenance(struct XaInferContext *ctx, struct XaSymbol *symbol,
                                          struct AstNode *value, struct XrType *value_type);
XR_FUNC void xa_check_pointer_borrow_escape(struct XaInferContext *ctx,
                                            struct AstNode *location_node, struct AstNode *value,
                                            struct XrType *value_type, const char *escape_context);
XR_FUNC const char *xa_address_kind_name(XaAddressKind kind);
XR_FUNC const char *xa_address_reject_reason_name(XaAddressRejectReason reason);

#endif /* XADDRESSABILITY_H */
