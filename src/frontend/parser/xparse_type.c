/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xparse_type.c - Type annotation parsing
 *
 * KEY CONCEPT:
 *   Parses type annotations: int, Array<int>, Map<string, int>, etc.
 */

#include "xparse_internal.h"
#include "../../base/xchecks.h"
#include "../../base/xarena.h"
#include "../../runtime/value/xtype_names.h"
#include "xtype_scope.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/xisolate_api.h"
#include <stdlib.h>
#include <string.h>
#include "../../base/xmalloc.h"

/* ========== Helper Functions ========== */

// Consume '>' in generic type context, handling '>>' (TK_RSHIFT) split
// When parsing Array<Array<int>>, the lexer tokenizes '>>' as TK_RSHIFT
// This function splits it into two '>' tokens
static bool consume_gt_in_generic(Parser *parser) {
    XR_DCHECK(parser != NULL, "consume_gt_in_generic: NULL parser");
    if (xr_parser_match(parser, TK_GT)) {
        return true;
    }

    // Handle '>>' being lexed as TK_RSHIFT
    if (parser->current.type == TK_RSHIFT) {
        // Consume the '>>' but leave one '>' for the outer generic
        parser->previous = parser->current;
        parser->previous.type = TK_GT;  // Record that we consumed a '>'

        // Transform current token from '>>' to '>'
        parser->current.type = TK_GT;
        parser->current.start++;  // Move past first '>'
        parser->current.length = 1;
        return true;
    }

    xr_parser_error(parser, "expected '>' (at '>>')");
    return false;
}

/* ========== Type Annotation Parsing (returns XrType) ========== */

// Parse type annotation to XrType
// Uses direct Token type comparison for efficiency (no string operations)
//
// Supported syntax:
// - Basic types: int, float, string, bool
// - Collection types: Array<T>, Map<K,V>, Set<T>
// - Special types: void, null
// - User-defined types / class names
// - Function types: fn(int, int): int
// - Optional types: int?, string?, Array<int>?
static XrType *parse_type_annotation_base(Parser *parser);

XrType *xr_parse_type_annotation(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_type_annotation: NULL parser");
    XrType *base = parse_type_annotation_base(parser);

    // Optional type suffix: Type?
    // All types can be nullable. For primitive types (int?, float?, bool?),
    // xr_type_to_slot_type returns XR_SLOT_ANY because raw storage cannot
    // distinguish null from a valid integer/float value.
    if (xr_parser_match(parser, TK_QUESTION)) {
        // Json already includes null — Json? is redundant and forbidden
        if (base && base->kind == XR_KIND_JSON) {
            xr_parser_error(parser,
                            "'Json?' is not allowed — Json already includes null as a valid value. "
                            "Use 'Json' instead.");
        } else {
            base = xr_type_new_optional(parser->X, base);
        }
    }

    // Union type: Type | Type | ...
    // e.g. int | string, int? | bool, {id: int} | Error
    if (xr_parser_check(parser, TK_PIPE)) {
        XrType *members[XR_UNION_MAX_MEMBERS];
        int count = 0;
        members[count++] = base;

        while (xr_parser_match(parser, TK_PIPE) && count < XR_UNION_MAX_MEMBERS + 1) {
            XrType *next = parse_type_annotation_base(parser);
            // Reject nested union alias at parse time
            if (XR_TYPE_IS_UNION(next)) {
                xr_parser_error(
                    parser,
                    "nested union alias not allowed in union type, expand members directly");
                return xr_type_new_unknown(NULL);
            }
            // Allow ? on individual member: int | string?
            if (xr_parser_match(parser, TK_QUESTION)) {
                next = xr_type_new_optional(parser->X, next);
            }
            if (count < XR_UNION_MAX_MEMBERS + 1)
                members[count++] = next;
        }

        if (count > XR_UNION_MAX_MEMBERS) {
            xr_parser_error(parser, "union type exceeds maximum of 6 members");
            return xr_type_new_unknown(NULL);
        }

        return xr_type_new_union(parser->X, members, count);
    }

    return base;
}

static XrType *parse_type_annotation_base(Parser *parser) {
    XR_DCHECK(parser != NULL, "parse_type_annotation_base: NULL parser");
    (void) parser->X;  // XrayIsolate no longer needed for XrType creation

    // Fixed-length array type: [N]T (compile-time length, runtime Array<T>)
    if (xr_parser_check(parser, TK_LBRACKET)) {
        // Peek ahead: [number] means fixed-array type; otherwise fall through
        Scanner saved = parser->scanner;
        Token saved_current = parser->current;
        xr_parser_advance(parser);  // consume '['
        if (parser->current.type == TK_LITERAL_INT) {
            int length = (int) strtol(parser->current.start, NULL, 10);
            xr_parser_advance(parser);  // consume number
            if (xr_parser_match(parser, TK_RBRACKET)) {
                XrType *elem = parse_type_annotation_base(parser);
                return xr_type_new_fixed_array(parser->X, elem, length);
            }
        }
        // Not a fixed-array type, restore parser state
        parser->scanner = saved;
        parser->current = saved_current;
    }

    // Basic type keywords - direct Token type matching
    if (xr_parser_match(parser, TK_INT)) {
        return xr_type_new_int(NULL);
    }
    if (xr_parser_match(parser, TK_FLOAT)) {
        return xr_type_new_float(NULL);
    }
    if (xr_parser_match(parser, TK_STRING)) {
        return xr_type_new_string(NULL);
    }
    if (xr_parser_match(parser, TK_BOOL)) {
        return xr_type_new_bool(NULL);
    }
    if (xr_parser_match(parser, TK_VOID)) {
        return xr_type_new_void(NULL);
    }
    if (xr_parser_match(parser, TK_NULL)) {
        return xr_type_new_null(NULL);
    }

    // Native-width integer types
    if (xr_parser_match(parser, TK_INT8))
        return xr_type_new_int_width(parser->X, XR_NATIVE_I8);
    if (xr_parser_match(parser, TK_INT16))
        return xr_type_new_int_width(parser->X, XR_NATIVE_I16);
    if (xr_parser_match(parser, TK_INT32))
        return xr_type_new_int_width(parser->X, XR_NATIVE_I32);
    if (xr_parser_match(parser, TK_INT64))
        return xr_type_new_int_width(parser->X, XR_NATIVE_I64);
    if (xr_parser_match(parser, TK_UINT8))
        return xr_type_new_int_width(parser->X, XR_NATIVE_U8);
    if (xr_parser_match(parser, TK_UINT16))
        return xr_type_new_int_width(parser->X, XR_NATIVE_U16);
    if (xr_parser_match(parser, TK_UINT32))
        return xr_type_new_int_width(parser->X, XR_NATIVE_U32);
    if (xr_parser_match(parser, TK_UINT64))
        return xr_type_new_int_width(parser->X, XR_NATIVE_U64);
    if (xr_parser_match(parser, TK_FLOAT32))
        return xr_type_new_float_width(parser->X, XR_NATIVE_F32);
    if (xr_parser_match(parser, TK_FLOAT64))
        return xr_type_new_float_width(parser->X, XR_NATIVE_F64);

    // Array<T> — generic parameter is mandatory in type annotations
    if (xr_parser_match(parser, TK_TYPE_ARRAY)) {
        if (!xr_parser_match(parser, TK_LT)) {
            if (!parser->allow_bare_container)
                xr_parser_error(parser, "Array requires a type parameter, e.g. Array<int>");
            return xr_type_new_array(parser->X, xr_type_new_unknown(NULL));
        }
        XrType *elem_type = xr_parse_type_annotation(parser);
        consume_gt_in_generic(parser);
        return xr_type_new_array(parser->X, elem_type);
    }

    // Map<K, V> — generic parameters are mandatory in type annotations
    if (xr_parser_match(parser, TK_TYPE_MAP)) {
        if (!xr_parser_match(parser, TK_LT)) {
            if (!parser->allow_bare_container)
                xr_parser_error(parser, "Map requires type parameters, e.g. Map<string, int>");
            return xr_type_new_map(parser->X, xr_type_new_unknown(NULL), xr_type_new_unknown(NULL));
        }
        XrType *key_type = xr_parse_type_annotation(parser);
        XrType *val_type = xr_type_new_unknown(NULL);
        if (xr_parser_match(parser, TK_COMMA)) {
            val_type = xr_parse_type_annotation(parser);
        }
        consume_gt_in_generic(parser);
        return xr_type_new_map(parser->X, key_type, val_type);
    }

    // Set<T> — generic parameter is mandatory in type annotations
    if (xr_parser_match(parser, TK_TYPE_SET)) {
        if (!xr_parser_match(parser, TK_LT)) {
            if (!parser->allow_bare_container)
                xr_parser_error(parser, "Set requires a type parameter, e.g. Set<int>");
            return xr_type_new_set(parser->X, xr_type_new_unknown(NULL));
        }
        XrType *elem_type = xr_parse_type_annotation(parser);
        consume_gt_in_generic(parser);
        return xr_type_new_set(parser->X, elem_type);
    }

    // Json dynamic object type
    if (xr_parser_match(parser, TK_TYPE_JSON)) {
        return xr_type_new_json(NULL);
    }

    // BigInt type
    if (xr_parser_match(parser, TK_TYPE_BIGINT)) {
        return xr_type_new_bigint(parser->X);
    }

    // DateTime type
    if (xr_parser_match(parser, TK_TYPE_DATETIME)) {
        return xr_type_new_datetime(parser->X);
    }

    // Bytes type
    if (xr_parser_match(parser, TK_TYPE_BYTES)) {
        return xr_type_new_bytes(parser->X);
    }

    // Range type
    if (xr_parser_match(parser, TK_TYPE_RANGE)) {
        XrType *t = xr_type_new(parser->X, XR_KIND_INSTANCE);
        if (t)
            t->instance.class_name = "Range";
        return t;
    }

    // Struct type literal: { x: float, y: float } or { x: float, ... }
    if (xr_parser_match(parser, TK_LBRACE)) {
        // Dynamic array, supports arbitrary fields (initial capacity 16, expands as needed)
        int capacity = 16;
        int field_count = 0;
        bool allow_extension =
            false;  // Whether extension is allowed (set true when ... is encountered)
        const char **field_names = xr_malloc(capacity * sizeof(const char *));
        XrType **field_types = xr_malloc(capacity * sizeof(XrType *));

        while (!xr_parser_check(parser, TK_RBRACE) && !xr_parser_check(parser, TK_EOF)) {
            // Check ... extensibility marker
            if (xr_parser_match(parser, TK_DOT_DOT_DOT)) {
                allow_extension = true;
                xr_parser_match(parser, TK_COMMA);  // Optional comma
                continue;                           // ... is not a field, continue parsing
            }

            // Dynamic expansion
            if (field_count >= capacity) {
                int new_cap = capacity * 2;
                XR_REALLOC_OR_ABORT(field_names, (size_t) new_cap * sizeof(const char *),
                                    "parse_type field_names grow");
                XR_REALLOC_OR_ABORT(field_types, (size_t) new_cap * sizeof(XrType *),
                                    "parse_type field_types grow");
                capacity = new_cap;
            }

            // Check readonly field syntax: const field: type (ignored for XrType)
            xr_parser_match(parser, TK_CONST);

            // Field name
            xr_parser_consume(parser, TK_NAME, "expected field name");
            field_names[field_count] = strndup(parser->previous.start, parser->previous.length);

            // Check optional field syntax: field?: type
            bool is_optional = xr_parser_match(parser, TK_QUESTION);

            xr_parser_consume(parser, TK_COLON, "expected ':'");

            // Field type (recursive parse)
            XrType *ftype = xr_parse_type_annotation(parser);

            // Optional field wrapped with optional type
            if (is_optional) {
                ftype = xr_type_new_optional(parser->X, ftype);
            }
            field_types[field_count] = ftype;
            field_count++;

            // Optional comma
            xr_parser_match(parser, TK_COMMA);
        }

        xr_parser_consume(parser, TK_RBRACE, "expected '}'");

        // With ... → dynamic Json type (extensible), without → fixed Object type
        XrType *result =
            allow_extension
                ? xr_type_new_json_with_fields(parser->X, field_names, field_types, field_count)
                : xr_type_new_object(parser->X, field_names, field_types, field_count, NULL);

        // Free temporary arrays (field_names strings are copied by xr_type_new_object)
        for (int i = 0; i < field_count; i++) {
            xr_free((void *) field_names[i]);
        }
        xr_free(field_names);
        xr_free(field_types);

        return result;
    }

    // Function type: fn(int, int): int
    if (xr_parser_check(parser, TK_FN)) {
        Scanner saved = parser->scanner;
        Token saved_current = parser->current;
        xr_parser_advance(parser);  // consume 'fn'
        if (xr_parser_check(parser, TK_LPAREN)) {
            xr_parser_advance(parser);  // consume '('
            XrType *types[16];
            int count = 0;

            while (!xr_parser_check(parser, TK_RPAREN) && !xr_parser_check(parser, TK_EOF)) {
                if (count > 0) {
                    xr_parser_match(parser, TK_COMMA);
                }
                if (count < 16) {
                    types[count++] = xr_parse_type_annotation(parser);
                }
            }

            xr_parser_consume(parser, TK_RPAREN, "expected ')'");

            // Parse return type after ':'
            XrType *return_type = xr_type_new_void(NULL);
            if (xr_parser_match(parser, TK_COLON)) {
                return_type = xr_parse_type_annotation(parser);
            }

            return xr_type_new_function(parser->X, types, count, return_type, false);
        } else {
            // Not a function type, restore parser state
            parser->scanner = saved;
            parser->current = saved_current;
        }
    }

    // Tuple type (multi-value return): (int, bool)
    if (xr_parser_match(parser, TK_LPAREN)) {
        XrType *types[16];
        int count = 0;

        while (!xr_parser_check(parser, TK_RPAREN) && !xr_parser_check(parser, TK_EOF)) {
            if (count > 0) {
                xr_parser_match(parser, TK_COMMA);
            }
            if (count < 16) {
                types[count++] = xr_parse_type_annotation(parser);
            }
        }

        xr_parser_consume(parser, TK_RPAREN, "expected ')'");
        return xr_type_new_tuple(parser->X, types, count);
    }

    // Channel<T> — generic parameter is mandatory in type annotations
    if (xr_parser_match(parser, TK_TYPE_CHANNEL)) {
        if (!xr_parser_match(parser, TK_LT)) {
            if (!parser->allow_bare_container)
                xr_parser_error(parser, "Channel requires a type parameter, e.g. Channel<int>");
            return xr_type_new_channel(parser->X, xr_type_new_unknown(NULL));
        }
        XrType *elem_type = xr_parse_type_annotation(parser);
        consume_gt_in_generic(parser);
        return xr_type_new_channel(parser->X, elem_type);
    }

    // Native-width integer types (first-class keywords)
    if (xr_parser_match(parser, TK_INT8))
        return xr_type_new_int_width(parser->X, XR_NATIVE_I8);
    if (xr_parser_match(parser, TK_INT16))
        return xr_type_new_int_width(parser->X, XR_NATIVE_I16);
    if (xr_parser_match(parser, TK_INT32))
        return xr_type_new_int_width(parser->X, XR_NATIVE_I32);
    if (xr_parser_match(parser, TK_INT64))
        return xr_type_new_int_width(parser->X, XR_NATIVE_I64);
    if (xr_parser_match(parser, TK_UINT8))
        return xr_type_new_int_width(parser->X, XR_NATIVE_U8);
    if (xr_parser_match(parser, TK_UINT16))
        return xr_type_new_int_width(parser->X, XR_NATIVE_U16);
    if (xr_parser_match(parser, TK_UINT32))
        return xr_type_new_int_width(parser->X, XR_NATIVE_U32);
    if (xr_parser_match(parser, TK_UINT64))
        return xr_type_new_int_width(parser->X, XR_NATIVE_U64);

    // Native-width float types
    if (xr_parser_match(parser, TK_FLOAT32))
        return xr_type_new_float_width(parser->X, XR_NATIVE_F32);
    if (xr_parser_match(parser, TK_FLOAT64))
        return xr_type_new_float_width(parser->X, XR_NATIVE_F64);

    // User-defined type or type alias (with optional generic parameters)
    if (xr_parser_match(parser, TK_NAME)) {
        Token name_token = parser->previous;
        char temp_name[256];
        int name_len = name_token.length < 255 ? name_token.length : 255;
        strncpy(temp_name, name_token.start, name_len);
        temp_name[name_len] = '\0';

        // JsonValue has been removed — use Json instead
        if (strcmp(temp_name, "JsonValue") == 0) {
            xr_parser_error(parser, "Type 'JsonValue' has been removed. Use 'Json' instead.");
            return xr_type_new_json(parser->X);
        }

        // Built-in instance types — must create XR_KIND_INSTANCE (not XR_KIND_CLASS)
        // so that xr_type_is_named_class() matches correctly downstream.
        if (strcmp(temp_name, "Task") == 0) {
            // Task<T> — optional generic parameter for result type
            XrType *result_type = xr_type_new_unknown(NULL);
            if (xr_parser_match(parser, TK_LT)) {
                result_type = xr_parse_type_annotation(parser);
                consume_gt_in_generic(parser);
            }
            return xr_type_new_task(parser->X, result_type);
        }
        if (strcmp(temp_name, "BigInt") == 0)
            return xr_type_new_bigint(parser->X);
        if (strcmp(temp_name, "Regex") == 0)
            return xr_type_new_regex(parser->X);
        if (strcmp(temp_name, "StringBuilder") == 0)
            return xr_type_new_stringbuilder(parser->X);
        if (strcmp(temp_name, "DateTime") == 0)
            return xr_type_new_datetime(parser->X);
        if (strcmp(temp_name, "Exception") == 0)
            return xr_type_new_named_instance(parser->X, "Exception");

        // 'any' type has been removed from xray.
        // Use concrete types or Json for dynamic values.
        if (strcmp(temp_name, "any") == 0) {
            xr_parser_error(parser, "'any' type is not supported. "
                                    "Use a concrete type or 'Json' for dynamic values.");
            return xr_type_new_unknown(NULL);
        }

        // Detect common type name misspellings from other languages
        if (strcmp(temp_name, "String") == 0 || strcmp(temp_name, "str") == 0) {
            xr_parser_error(parser, "type 'string' must be lowercase in Xray");
            return xr_type_new_string(NULL);
        }
        if (strcmp(temp_name, "Int") == 0 || strcmp(temp_name, "Integer") == 0 ||
            strcmp(temp_name, "integer") == 0) {
            xr_parser_error(parser, "use 'int' (lowercase) for integer type in Xray");
            return xr_type_new_int(NULL);
        }
        if (strcmp(temp_name, "Float") == 0 || strcmp(temp_name, "Double") == 0 ||
            strcmp(temp_name, "double") == 0) {
            xr_parser_error(parser, "use 'float' (lowercase) for floating-point type in Xray");
            return xr_type_new_float(NULL);
        }
        if (strcmp(temp_name, "Bool") == 0 || strcmp(temp_name, "Boolean") == 0 ||
            strcmp(temp_name, "boolean") == 0) {
            xr_parser_error(parser, "use 'bool' (lowercase) for boolean type in Xray");
            return xr_type_new_bool(NULL);
        }
        if (strcmp(temp_name, "char") == 0 || strcmp(temp_name, "Char") == 0) {
            xr_parser_error(parser, "there is no 'char' type in Xray. Use 'string' for characters");
            return xr_type_new_string(NULL);
        }
        if (strcmp(temp_name, "void") == 0) {
            // 'void' as TK_NAME means it wasn't recognized as keyword in this context
            xr_parser_error(parser, "use 'void' only as function return type");
            return xr_type_new_unknown(NULL);
        }

        // Lookup type alias
        if (parser->type_scope) {
            XrTypeAlias *alias = xr_type_scope_lookup(parser->type_scope, temp_name);
            if (alias && alias->type) {
                // The entry's name pointer is heap-allocated by the scope and
                // lives until xr_type_scope_free, so it is safe to keep.
                alias->type->alias_name = alias->name;
                return alias->type;
            }
        }

        // Parse optional generic type parameters: Box<int>, Pair<K, V>
        if (xr_parser_match(parser, TK_LT)) {
            // Collect type arguments
            XrType *type_args[16];  // Max 16 type arguments
            int type_arg_count = 0;

            do {
                if (type_arg_count < 16) {
                    type_args[type_arg_count++] = xr_parse_type_annotation(parser);
                }
            } while (xr_parser_match(parser, TK_COMMA));

            consume_gt_in_generic(parser);

            // Create generic instance type with type arguments
            XrType **args_copy = NULL;
            if (type_arg_count > 0) {
                args_copy = xr_malloc(sizeof(XrType *) * type_arg_count);
                for (int i = 0; i < type_arg_count; i++) {
                    args_copy[i] = type_args[i];
                }
            }
            return xr_type_new_generic_instance(parser->X, temp_name, NULL, args_copy,
                                                type_arg_count);
        }

        return xr_type_new_class(parser->X, temp_name);
    }

    // Error recovery
    xr_parser_error_expected_name(parser, "expected type name");
    return xr_type_new_unknown(NULL);
}
