/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_op_name.h - Human-readable Xi op names for diagnostics
 *
 * Pure lookup with no side effects. Usable from dump, verify, and
 * any diagnostic context that needs to print an op name.
 */

#ifndef XI_OP_NAME_H
#define XI_OP_NAME_H

#include "xi.h"

static inline const char *xi_op_name(uint16_t op) {
    switch (op) {
        case XI_CONST:         return "CONST";
        case XI_PARAM:         return "PARAM";
        case XI_ADD:           return "ADD";
        case XI_SUB:           return "SUB";
        case XI_MUL:           return "MUL";
        case XI_DIV:           return "DIV";
        case XI_MOD:           return "MOD";
        case XI_NEG:           return "NEG";
        case XI_BAND:          return "BAND";
        case XI_BOR:           return "BOR";
        case XI_BXOR:          return "BXOR";
        case XI_BNOT:          return "BNOT";
        case XI_SHL:           return "SHL";
        case XI_SHR:           return "SHR";
        case XI_EQ:            return "EQ";
        case XI_NE:            return "NE";
        case XI_LT:            return "LT";
        case XI_LE:            return "LE";
        case XI_GT:            return "GT";
        case XI_GE:            return "GE";
        case XI_EQ_STRICT:     return "EQ_STRICT";
        case XI_NE_STRICT:     return "NE_STRICT";
        case XI_NOT:           return "NOT";
        case XI_CONVERT:       return "CONVERT";
        case XI_BOX:           return "BOX";
        case XI_UNBOX:         return "UNBOX";
        case XI_LOAD_FIELD:    return "LOAD_FIELD";
        case XI_STORE_FIELD:   return "STORE_FIELD";
        case XI_INDEX_GET:     return "INDEX_GET";
        case XI_INDEX_SET:     return "INDEX_SET";
        case XI_JSON_NEW:      return "JSON_NEW";
        case XI_JSON_INIT_F:   return "JSON_INIT_F";
        case XI_JSON_GET_F:    return "JSON_GET_F";
        case XI_JSON_SET_F:    return "JSON_SET_F";
        case XI_JSON_DECODE:   return "JSON_DECODE";
        case XI_ARRAY_NEW:     return "ARRAY_NEW";
        case XI_MAP_NEW:       return "MAP_NEW";
        case XI_CALL:          return "CALL";
        case XI_CALL_METHOD:   return "CALL_METHOD";
        case XI_CALL_BUILTIN:  return "CALL_BUILTIN";
        case XI_EXTRACT:       return "EXTRACT";
        case XI_CLOSURE_NEW:   return "CLOSURE_NEW";
        case XI_LOAD_UPVAL:    return "LOAD_UPVAL";
        case XI_STORE_UPVAL:   return "STORE_UPVAL";
        case XI_GET_SHARED:    return "GET_SHARED";
        case XI_SET_SHARED:    return "SET_SHARED";
        case XI_PRINT:         return "PRINT";
        case XI_GO:            return "GO";
        case XI_AWAIT:         return "AWAIT";
        case XI_CHAN_SEND:     return "CHAN_SEND";
        case XI_CHAN_RECV:     return "CHAN_RECV";
        case XI_CHAN_TRY_SEND: return "CHAN_TRY_SEND";
        case XI_CHAN_TRY_RECV: return "CHAN_TRY_RECV";
        case XI_YIELD:         return "YIELD";
        case XI_THROW:         return "THROW";
        case XI_ITER_NEW:      return "ITER_NEW";
        case XI_ITER_NEXT:     return "ITER_NEXT";
        case XI_ITER_VALID:    return "ITER_VALID";
        case XI_DEFER:         return "DEFER";
        case XI_CHAN_NEW:      return "CHAN_NEW";
        case XI_SET_NEW:       return "SET_NEW";
        case XI_STR_CONCAT:    return "STR_CONCAT";
        case XI_IS:            return "IS";
        case XI_AS:            return "AS";
        case XI_SLICE:         return "SLICE";
        case XI_RANGE:         return "RANGE";
        case XI_MULTI_RET:     return "MULTI_RET";
        case XI_ISNULL:        return "ISNULL";
        case XI_PHI:           return "PHI";
        case XI_COPY:          return "COPY";
        case XI_CLASS_CREATE:  return "CLASS_CREATE";
        case XI_SCOPE_ENTER:   return "SCOPE_ENTER";
        case XI_SCOPE_EXIT:    return "SCOPE_EXIT";
        case XI_TRY:           return "TRY";
        case XI_CATCH:         return "CATCH";
        case XI_FINALLY:       return "FINALLY";
        case XI_END_TRY:       return "END_TRY";
        case XI_ASSERT:        return "ASSERT";
        case XI_ASSERT_EQ:     return "ASSERT_EQ";
        case XI_ASSERT_NE:     return "ASSERT_NE";
        case XI_ASSERT_THROWS: return "ASSERT_THROWS";
        case XI_TYPEOF:        return "TYPEOF";
        case XI_GET_BUILTIN:   return "GET_BUILTIN";
        case XI_IMPORT_REF:    return "IMPORT_REF";
        case XI_REGEX_COMPILE: return "REGEX_COMPILE";
        case XI_RETAIN:        return "RETAIN";
        case XI_RELEASE:       return "RELEASE";
        case XI_MOVE:          return "MOVE";
        default:               return "???";
    }
}

#endif  // XI_OP_NAME_H
