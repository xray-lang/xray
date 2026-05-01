/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xi_cgen.c - Xi IR to C code generation
 *
 * Walks XiFunc -> XiBlock -> XiValue and emits equivalent C code.
 *
 * Strategy:
 *   - All locals declared at function top (C89 style) to avoid
 *     scope issues across labels.
 *   - Each basic block emits a label (L0:, L1:, ...).
 *   - PHI nodes are eliminated by inserting assignments before
 *     jumps in predecessor blocks.
 *   - Value representation (I64/F64/TAGGED) determined by op+type,
 *     same logic as xi_opt_select_rep.
 */

#include "xi_cgen.h"
#include "xi_rep.h"
#include "../base/xdefs.h"
#include "../base/xchecks.h"
#include "../runtime/value/xtype.h"
#include "../aot/xrt_method_symbols.h"
#include "../frontend/parser/xast_nodes.h"
#include <string.h>
#include <inttypes.h>

/* Iterator/hasNext/next symbols not in xrt_method_symbols.h (no runtime
 * method dispatch for these — the VM uses OP_INVOKE with dynamic lookup).
 * Define local placeholders matching SYMBOL_ITERATOR(14)/HAS_NEXT(15)/NEXT(16). */
#ifndef XRT_SYM_ITERATOR
#define XRT_SYM_ITERATOR 14
#endif
#ifndef XRT_SYM_HAS_NEXT
#define XRT_SYM_HAS_NEXT 15
#endif
#ifndef XRT_SYM_NEXT
#define XRT_SYM_NEXT 16
#endif

/* ========== Representation Helpers ========== */

/* Shared implementation — see xi_rep.h */
#define cg_rep xi_value_def_rep

static const char *ctype_str(XrRep rep) {
    switch (rep) {
        case XR_REP_I64: return "int64_t";
        case XR_REP_F64: return "double";
        default:         return "XrValue";
    }
}

/* ========== C Name Helpers ========== */

/* Global counter to disambiguate anonymous/duplicate function names.
 * Incremented each time a C function name is first assigned. */
static int g_fname_counter;

/* ========== Shared Slot → XiFunc Mapping ========== */

/* Populated by xi_cgen_program() before emission.
 * Maps shared-variable index → XiFunc* for module-level functions
 * and class constructors stored via XI_SET_SHARED(slot, XI_CLOSURE_NEW). */
#define CG_MAX_SHARED 512
static const XiFunc *g_shared_funcs[CG_MAX_SHARED];
static const XiClassData *g_shared_class[CG_MAX_SHARED];
static int g_nshared;

/* Instance method registry: maps (method_name → XiFunc*).
 * Populated by prescan from XI_CLASS_CREATE descriptors so that
 * XI_CALL_METHOD can resolve class methods to direct calls. */
#define CG_MAX_METHODS 256
typedef struct {
    const char *class_name;  /* owning class (e.g. "Rect") */
    const char *name;        /* method name (e.g. "area") */
    const XiFunc *func;
} CgMethodEntry;
static CgMethodEntry g_methods[CG_MAX_METHODS];
static int g_nmethod;

/* Module-level function pointer (set by prescan) for class lookups */
static const XiFunc *g_module_func;

/* Set when current function has exception handling; all SSA values
 * are pre-declared so inline emit uses assignment instead of decl. */
static bool g_pre_decl_all;

/* Name of the current module's shared array in generated C.
 * Single-module: "xrt_shared".  Multi-module: "xrt_shared_<modname>". */
static const char *g_shared_name = "xrt_shared";

/* Prescan a function's IR to populate g_shared_funcs.
 * Recognizes XI_SET_SHARED(slot, XI_CLOSURE_NEW(child)) pattern,
 * including XI_SET_SHARED(slot, XI_BOX(XI_CLOSURE_NEW(child))). */
/* Find the constructor child XiFunc from a XiClassData descriptor.
 * The constructor is the instance method named "constructor". */
static const XiFunc *cg_find_constructor(const XiFunc *parent,
                                          const XiClassData *cd) {
    if (!cd || !cd->ast || !parent) return NULL;
    const ClassDeclNode *cls = &cd->ast->as.class_decl;
    uint16_t ci = 0;
    for (int i = 0; i < cls->method_count; i++) {
        if (cls->methods[i]->type != AST_METHOD_DECL) continue;
        const MethodDeclNode *m = &cls->methods[i]->as.method_decl;
        if (m->is_static_constructor) continue;
        if (m->is_constructor || (m->name && strcmp(m->name, "constructor") == 0)) {
            if (cd->child_idx && ci < cd->ninst + cd->nstat) {
                uint16_t idx = cd->child_idx[ci];
                if (idx < parent->nchildren)
                    return parent->children[idx];
            }
        }
        ci++;
    }
    return NULL;
}

/* Register all instance methods from a class descriptor into g_methods.
 * Constructors are excluded — they are resolved via the XI_CALL class path.
 * Also records the constructor XiFunc for super() call resolution. */
static void cg_register_class_methods(const XiFunc *parent,
                                       const XiClassData *cd) {
    if (!cd || !cd->ast || !parent) return;
    const ClassDeclNode *cls = &cd->ast->as.class_decl;
    uint16_t ci = 0;
    for (int i = 0; i < cls->method_count; i++) {
        if (cls->methods[i]->type != AST_METHOD_DECL) continue;
        const MethodDeclNode *m = &cls->methods[i]->as.method_decl;
        if (m->is_static_constructor) continue;
        bool is_ctor = m->is_constructor
                       || (m->name && strcmp(m->name, "constructor") == 0);
        if (!is_ctor && !m->is_static && m->name) {
            if (cd->child_idx && ci < cd->ninst + cd->nstat) {
                uint16_t idx = cd->child_idx[ci];
                if (idx < parent->nchildren && g_nmethod < CG_MAX_METHODS) {
                    g_methods[g_nmethod].class_name = cls->name;
                    g_methods[g_nmethod].name = m->name;
                    g_methods[g_nmethod].func = parent->children[idx];
                    g_nmethod++;
                }
            }
        }
        ci++;
    }
}

/* Lookup constructor XiFunc for a class by name.
 * Used to resolve super() calls to the parent class constructor. */
static const XiFunc *cg_lookup_class_ctor(const XiFunc *parent,
                                           const char *class_name) {
    if (!class_name || !parent) return NULL;
    for (uint32_t bi = 0; bi < parent->nblocks; bi++) {
        const XiBlock *blk = parent->blocks[bi];
        if (!blk) continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v || v->op != XI_CLASS_CREATE || !v->aux) continue;
            const XiClassData *cd = (const XiClassData *)v->aux;
            if (!cd->ast) continue;
            const ClassDeclNode *cls = &cd->ast->as.class_decl;
            if (cls->name && strcmp(cls->name, class_name) == 0)
                return cg_find_constructor(parent, cd);
        }
    }
    return NULL;
}

/* Lookup a class instance method by name, optionally filtered by class.
 * If class_name is non-NULL, only match methods of that class.
 * If class_name is NULL, return the last match (most-derived class
 * is registered last due to topo-order scan). */
static const XiFunc *cg_lookup_method(const char *name,
                                       const char *class_name) {
    if (!name) return NULL;
    const XiFunc *last_match = NULL;
    for (int i = 0; i < g_nmethod; i++) {
        if (!g_methods[i].name || strcmp(g_methods[i].name, name) != 0)
            continue;
        if (class_name && g_methods[i].class_name &&
            strcmp(g_methods[i].class_name, class_name) == 0)
            return g_methods[i].func;
        last_match = g_methods[i].func;
    }
    return class_name ? NULL : last_match;
}

static void cg_prescan_shared(const XiFunc *f) {
    memset(g_shared_funcs, 0, sizeof(g_shared_funcs));
    memset(g_shared_class, 0, sizeof(g_shared_class));
    g_nshared = f->nshared;
    g_nmethod = 0;
    g_module_func = f;

    /* Also scan direct CLASS_CREATE values (not via SET_SHARED) */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk) continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v) continue;

            /* Register class methods from any CLASS_CREATE in the IR */
            if (v->op == XI_CLASS_CREATE && v->aux) {
                cg_register_class_methods(f, (const XiClassData *)v->aux);
            }

            if (v->op != XI_SET_SHARED) continue;
            int slot = (int)v->aux_int;
            if (slot < 0 || slot >= CG_MAX_SHARED || v->nargs < 1) continue;
            const XiValue *src = v->args[0];
            /* Direct: SET_SHARED(slot, CLOSURE_NEW(child)) */
            if (src->op == XI_CLOSURE_NEW && src->aux) {
                g_shared_funcs[slot] = (const XiFunc *)src->aux;
            }
            /* Boxed: SET_SHARED(slot, BOX(CLOSURE_NEW(child))) */
            else if (src->op == XI_BOX && src->nargs >= 1 &&
                     src->args[0]->op == XI_CLOSURE_NEW && src->args[0]->aux) {
                g_shared_funcs[slot] = (const XiFunc *)src->args[0]->aux;
            }
            /* Class: SET_SHARED(slot, CLASS_CREATE(data)) */
            else if (src->op == XI_CLASS_CREATE && src->aux) {
                const XiClassData *cd = (const XiClassData *)src->aux;
                g_shared_class[slot] = cd;
                const XiFunc *ctor = cg_find_constructor(f, cd);
                if (ctor)
                    g_shared_funcs[slot] = ctor;
            }
        }
    }
}

/* Write the C name for a function (prefix_funcname_id).
 * Each XiFunc gets a unique numeric suffix to prevent name collisions
 * (e.g. multiple anonymous closures or same-named constructors).
 * The suffix is stored in cgen_id the first time and reused thereafter. */
static void emit_fname(FILE *out, const char *prefix, const XiFunc *f) {
    XR_DCHECK(f != NULL, "emit_fname: NULL func");
    const char *raw = f->name ? f->name : "anon";

    /* Sanitize: replace non-alnum/underscore chars */
    char buf[128];
    size_t j = 0;
    for (const char *p = raw; *p && j < sizeof(buf) - 1; p++) {
        char c = *p;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_')
            buf[j++] = c;
        else
            buf[j++] = '_';
    }
    buf[j] = '\0';

    /* Assign a stable unique ID on first use (cgen_id == 0 means unassigned) */
    XiFunc *mf = (XiFunc *)(uintptr_t)f; /* cast away const for cgen_id write */
    if (mf->cgen_id == 0)
        mf->cgen_id = ++g_fname_counter;

    if (prefix && prefix[0])
        fprintf(out, "%s_%s_%d", prefix, buf, f->cgen_id);
    else
        fprintf(out, "fn_%s_%d", buf, f->cgen_id);
}

/* Write a value reference: v<id> or phi<id> for phi nodes */
static void emit_vref(FILE *out, const XiValue *v) {
    if (v->op == XI_PHI)
        fprintf(out, "phi%u", v->id);
    else
        fprintf(out, "v%u", v->id);
}

/* Write a phi variable reference: phi<id> */
static void emit_phi_ref(FILE *out, const XiPhi *phi) {
    fprintf(out, "phi%u", phi->value.id);
}

/* ========== Method Symbol Resolution ========== */

/* Map a method name string (from XI_CALL_METHOD/XI_LOAD_FIELD aux) to
 * the corresponding XRT_SYM_* integer. Returns -1 if not a known builtin. */
static int cg_method_sym(const char *name) {
    if (!name) return -1;
    /* Table-driven for the most common builtins */
    static const struct { const char *name; int sym; } map[] = {
        {"length",      XRT_SYM_LENGTH},
        {"size",        XRT_SYM_SIZE},
        {"isEmpty",     XRT_SYM_IS_EMPTY},
        {"has",         XRT_SYM_HAS},
        {"get",         XRT_SYM_GET},
        {"set",         XRT_SYM_SET},
        {"delete",      XRT_SYM_DELETE},
        {"clear",       XRT_SYM_CLEAR},
        {"keys",        XRT_SYM_KEYS},
        {"values",      XRT_SYM_VALUES},
        {"push",        XRT_SYM_PUSH},
        {"pop",         XRT_SYM_POP},
        {"shift",       XRT_SYM_SHIFT},
        {"unshift",     XRT_SYM_UNSHIFT},
        {"join",        XRT_SYM_JOIN},
        {"reverse",     XRT_SYM_REVERSE},
        {"slice",       XRT_SYM_SLICE},
        {"indexOf",     XRT_SYM_INDEXOF},
        {"contains",    XRT_SYM_CONTAINS},
        {"includes",    XRT_SYM_INCLUDES},
        {"startsWith",  XRT_SYM_STARTSWITH},
        {"endsWith",    XRT_SYM_ENDSWITH},
        {"toLowerCase", XRT_SYM_TOLOWER},
        {"toUpperCase", XRT_SYM_TOUPPER},
        {"charAt",      XRT_SYM_CHARAT},
        {"substring",   XRT_SYM_SUBSTRING},
        {"trim",        XRT_SYM_TRIM},
        {"trimStart",   XRT_SYM_TRIM_START},
        {"trimEnd",     XRT_SYM_TRIM_END},
        {"split",       XRT_SYM_SPLIT},
        {"replace",     XRT_SYM_REPLACE},
        {"replaceAll",  XRT_SYM_REPLACEALL},
        {"repeat",      XRT_SYM_REPEAT},
        {"concat",      XRT_SYM_CONCAT},
        {"byteAt",      XRT_SYM_BYTE_AT},
        {"padStart",    XRT_SYM_PAD_START},
        {"padEnd",      XRT_SYM_PAD_END},
        {"lastIndexOf", XRT_SYM_LASTINDEXOF},
        {"toInt",       XRT_SYM_TOINT},
        {"toFloat",     XRT_SYM_TOFLOAT},
        {"ord",         XRT_SYM_ORD},
        {"max",         XRT_SYM_MAX},
        {"min",         XRT_SYM_MIN},
        {"toHex",       XRT_SYM_TOHEX},
        {"fill",        XRT_SYM_FILL},
        {"sort",        XRT_SYM_SORT},
        {"floor",       XRT_SYM_FLOOR},
        {"ceil",        XRT_SYM_CEIL},
        {"round",       XRT_SYM_ROUND},
        {"abs",         XRT_SYM_ABS},
        {"sqrt",        XRT_SYM_SQRT},
        {"pow",         XRT_SYM_POW},
        {"toFixed",     XRT_SYM_TOFIXED},
        {"toString",    XRT_SYM_TOSTRING},
        {"add",         XRT_SYM_SET},  /* set.add() maps to SET symbol */
        {"iterator",    XRT_SYM_ITERATOR},
        {"hasNext",     XRT_SYM_HAS_NEXT},
        {"next",        XRT_SYM_NEXT},
        {"forEach",     XRT_SYM_FOREACH},
        {"map",         XRT_SYM_MAP},
        {"filter",      XRT_SYM_FILTER},
        {"reduce",      XRT_SYM_REDUCE},
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(name, map[i].name) == 0)
            return map[i].sym;
    }
    return -1;
}

/* ========== Value Emission ========== */

static void emit_binop(FILE *out, const XiValue *v, const char *op) {
    emit_vref(out, v->args[0]);
    fprintf(out, " %s ", op);
    emit_vref(out, v->args[1]);
}

/* Emit the RHS expression for a single value. */
static void emit_value_rhs(FILE *out, const XiFunc *f,
                             const XiValue *v, const char *prefix) {
    switch (v->op) {
        case XI_CONST:
            if (v->type->kind == XR_KIND_INT)
                fprintf(out, "INT64_C(%" PRId64 ")", v->aux_int);
            else if (v->type->kind == XR_KIND_FLOAT) {
                double d;
                memcpy(&d, &v->aux_int, sizeof(double));
                fprintf(out, "%a", d);
            } else if (v->type->kind == XR_KIND_BOOL)
                fprintf(out, "%" PRId64, v->aux_int);
            else if (v->type->kind == XR_KIND_NULL)
                fprintf(out, "XR_NULL_VAL");
            else if (v->type->kind == XR_KIND_STRING) {
                /* Emit escaped string literal wrapped in xrt_box_str */
                const char *s = (const char *)v->aux;
                fprintf(out, "xr_box_str(\"");
                if (s) {
                    for (const char *p = s; *p; p++) {
                        if (*p == '"') fprintf(out, "\\\"");
                        else if (*p == '\\') fprintf(out, "\\\\");
                        else if (*p == '\n') fprintf(out, "\\n");
                        else if (*p == '\t') fprintf(out, "\\t");
                        else fputc(*p, out);
                    }
                }
                fprintf(out, "\")");
            } else {
                fprintf(out, "XR_NULL_VAL /* unknown const kind */");
            }
            break;

        case XI_PARAM:
            fprintf(out, "p%u", (unsigned)v->aux_int);
            break;

        case XI_COPY:
            emit_vref(out, v->args[0]);
            break;

        /* Arithmetic: use C operators when both operands are scalar.
         * When any operand is tagged (XrValue), must use runtime functions.
         * When result rep is scalar but operands are mixed, box each tagged
         * operand before the C operation, or fall back to runtime dispatch. */
        case XI_ADD: case XI_SUB: case XI_MUL: case XI_DIV: case XI_MOD: {
            XrRep result_rep = cg_rep(v);
            XrRep a_rep = cg_rep(v->args[0]);
            XrRep b_rep = cg_rep(v->args[1]);
            bool any_tagged = (a_rep == XR_REP_TAGGED || b_rep == XR_REP_TAGGED);
            if (result_rep == XR_REP_TAGGED || any_tagged) {
                /* Use runtime dispatch — handles int/float/mixed correctly */
                const char *fn = NULL;
                switch (v->op) {
                    case XI_ADD: fn = "xrt_add"; break;
                    case XI_SUB: fn = "xrt_sub"; break;
                    case XI_MUL: fn = "xrt_mul"; break;
                    case XI_DIV: fn = "xrt_div"; break;
                    case XI_MOD: fn = "xrt_mod"; break;
                    default: break;
                }
                /* xrt_* returns XrValue.  If result_rep is scalar,
                 * extract with .f or .i after the runtime call. */
                if (result_rep == XR_REP_F64) {
                    fprintf(out, "%s(", fn);
                    if (a_rep != XR_REP_TAGGED) {
                        fprintf(out, "XR_FROM_FLOAT(");
                        emit_vref(out, v->args[0]);
                        fprintf(out, ")");
                    } else {
                        emit_vref(out, v->args[0]);
                    }
                    fprintf(out, ", ");
                    if (b_rep != XR_REP_TAGGED) {
                        fprintf(out, "XR_FROM_FLOAT(");
                        emit_vref(out, v->args[1]);
                        fprintf(out, ")");
                    } else {
                        emit_vref(out, v->args[1]);
                    }
                    fprintf(out, ").f");
                } else if (result_rep == XR_REP_I64) {
                    fprintf(out, "%s(", fn);
                    if (a_rep != XR_REP_TAGGED) {
                        fprintf(out, "XR_FROM_INT(");
                        emit_vref(out, v->args[0]);
                        fprintf(out, ")");
                    } else {
                        emit_vref(out, v->args[0]);
                    }
                    fprintf(out, ", ");
                    if (b_rep != XR_REP_TAGGED) {
                        fprintf(out, "XR_FROM_INT(");
                        emit_vref(out, v->args[1]);
                        fprintf(out, ")");
                    } else {
                        emit_vref(out, v->args[1]);
                    }
                    fprintf(out, ").i");
                } else {
                    fprintf(out, "%s(", fn);
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", ");
                    emit_vref(out, v->args[1]);
                    fprintf(out, ")");
                }
            } else {
                const char *op = "+";
                switch (v->op) {
                    case XI_SUB: op = "-"; break;
                    case XI_MUL: op = "*"; break;
                    case XI_DIV: op = "/"; break;
                    case XI_MOD: op = "%%"; break;
                    default: break;
                }
                emit_binop(out, v, op);
            }
            break;
        }
        case XI_NEG: {
            XrRep result_rep = cg_rep(v);
            XrRep a_rep = cg_rep(v->args[0]);
            if (result_rep == XR_REP_TAGGED || a_rep == XR_REP_TAGGED) {
                fprintf(out, "xrt_neg(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            } else {
                fprintf(out, "-");
                emit_vref(out, v->args[0]);
            }
            break;
        }

        /* Bitwise */
        case XI_BAND: emit_binop(out, v, "&"); break;
        case XI_BOR:  emit_binop(out, v, "|"); break;
        case XI_BXOR: emit_binop(out, v, "^"); break;
        case XI_BNOT:
            fprintf(out, "~");
            emit_vref(out, v->args[0]);
            break;
        case XI_SHL: emit_binop(out, v, "<<"); break;
        case XI_SHR: emit_binop(out, v, ">>"); break;

        /* Comparison: scalar comparison uses C operators; tagged uses xrt_eq/lt/le.
         * Result is always I64 (boolean 0/1). */
        case XI_EQ: case XI_NE: case XI_LT: case XI_LE: case XI_GT: case XI_GE: {
            XrRep a0_rep = cg_rep(v->args[0]);
            XrRep a1_rep = cg_rep(v->args[1]);
            XrRep arg_rep = (a0_rep == XR_REP_TAGGED || a1_rep == XR_REP_TAGGED)
                            ? XR_REP_TAGGED : a0_rep;
            if (arg_rep == XR_REP_TAGGED) {
                switch (v->op) {
                    case XI_EQ: fprintf(out, "xrt_eq("); break;
                    case XI_NE: fprintf(out, "!xrt_eq("); break;
                    case XI_LT: fprintf(out, "xrt_lt("); break;
                    case XI_LE: fprintf(out, "xrt_le("); break;
                    case XI_GT: fprintf(out, "xrt_lt("); break;
                    case XI_GE: fprintf(out, "xrt_le("); break;
                    default: break;
                }
                /* GT/GE: swap operands → xrt_lt(b,a) / xrt_le(b,a) */
                if (v->op == XI_GT || v->op == XI_GE) {
                    emit_vref(out, v->args[1]);
                    fprintf(out, ", ");
                    emit_vref(out, v->args[0]);
                } else {
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", ");
                    emit_vref(out, v->args[1]);
                }
                fprintf(out, ")");
            } else {
                const char *op = "==";
                switch (v->op) {
                    case XI_NE: op = "!="; break;
                    case XI_LT: op = "<"; break;
                    case XI_LE: op = "<="; break;
                    case XI_GT: op = ">"; break;
                    case XI_GE: op = ">="; break;
                    default: break;
                }
                emit_binop(out, v, op);
            }
            break;
        }

        /* Logical */
        case XI_NOT:
            fprintf(out, "!");
            emit_vref(out, v->args[0]);
            break;

        case XI_ISNULL:
            fprintf(out, "(");
            emit_vref(out, v->args[0]);
            fprintf(out, ".tag == XR_TAG_NULL)");
            break;

        /* Box / Unbox */
        case XI_BOX: {
            struct XrType *sty = v->args[0]->type;
            if (sty && sty->kind == XR_KIND_NULL) {
                /* Null is already tagged; no actual boxing needed */
                emit_vref(out, v->args[0]);
            } else if (sty && sty->kind == XR_KIND_FLOAT) {
                fprintf(out, "XR_FROM_FLOAT(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            } else if (sty && sty->kind == XR_KIND_BOOL) {
                fprintf(out, "XR_FROM_BOOL(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            } else if (sty && sty->kind == XR_KIND_STRING) {
                /* String is already tagged */
                emit_vref(out, v->args[0]);
            } else {
                fprintf(out, "XR_FROM_INT(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            }
            break;
        }

        case XI_UNBOX: {
            /* Determine unbox accessor based on result representation:
             * I64 → .i, F64 → .f, TAGGED → pass through as XrValue. */
            XrRep ur = cg_rep(v);
            emit_vref(out, v->args[0]);
            if (ur == XR_REP_F64)
                fprintf(out, ".f");
            else if (ur == XR_REP_I64)
                fprintf(out, ".i");
            /* else: TAGGED — no accessor, keep as XrValue */
            break;
        }

        /* Convert */
        case XI_CONVERT:
            if (v->type->kind == XR_KIND_FLOAT) {
                fprintf(out, "(double)");
                emit_vref(out, v->args[0]);
            } else if (v->type->kind == XR_KIND_INT) {
                fprintf(out, "(int64_t)");
                emit_vref(out, v->args[0]);
            } else {
                emit_vref(out, v->args[0]);
            }
            break;

        /* Print: aux_int encoding: bit0=add_space, bit1=newline */
        case XI_PRINT: {
            int flags = (int)v->aux_int;
            bool add_space = (flags & 1) != 0;
            bool newline   = (flags & 2) != 0;
            if (add_space)
                fprintf(out, "(putchar(' '), ");
            fprintf(out, "%s(", newline ? "xrt_println" : "xrt_print");
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
            if (add_space)
                fprintf(out, ")");
            break;
        }

        /* Function call: args[0]=callee, args[1..n]=params */
        case XI_CALL: {
            XiValue *callee = v->args[0];
            const XiFunc *target = NULL;

            /* Resolve callee to a known XiFunc:
             *   1. XI_CLOSURE_NEW with aux → direct child reference
             *   2. XI_BOX of XI_CLOSURE_NEW → boxed child reference
             *   3. XI_CONST null inside a named child → self-recursive call
             *   4. XI_GET_SHARED → module-level function via prescan table
             *   5. XI_UNBOX / XI_BOX wrappers around the above */
            if (callee->op == XI_CLOSURE_NEW && callee->aux) {
                target = (const XiFunc *)callee->aux;
            } else if (callee->op == XI_BOX &&
                       callee->args[0]->op == XI_CLOSURE_NEW &&
                       callee->args[0]->aux) {
                target = (const XiFunc *)callee->args[0]->aux;
            } else if (callee->op == XI_CONST &&
                       callee->type && callee->type->kind == XR_KIND_NULL &&
                       f->name) {
                /* Self-recursive call: lowerer stores null as self-ref */
                target = f;
            } else if (callee->op == XI_BOX &&
                       callee->args[0]->op == XI_CONST &&
                       callee->args[0]->type &&
                       callee->args[0]->type->kind == XR_KIND_NULL &&
                       f->name) {
                /* Self-recursive call wrapped by select_rep BOX */
                target = f;
            }
            /* GET_SHARED(slot) → lookup in prescan table */
            if (!target && callee->op == XI_GET_SHARED) {
                int slot = (int)callee->aux_int;
                if (slot >= 0 && slot < g_nshared)
                    target = g_shared_funcs[slot];
            }
            /* BOX(GET_SHARED(slot)) or UNBOX(GET_SHARED(slot)) */
            if (!target && (callee->op == XI_BOX || callee->op == XI_UNBOX) &&
                callee->nargs >= 1 && callee->args[0]->op == XI_GET_SHARED) {
                int slot = (int)callee->args[0]->aux_int;
                if (slot >= 0 && slot < g_nshared)
                    target = g_shared_funcs[slot];
            }

            /* Detect class constructor call.
             * Patterns:
             *   a) CALL(GET_SHARED(slot))       — slot has class data
             *   b) CALL(BOX/UNBOX(GET_SHARED))  — wrapped variant
             *   c) CALL(CLASS_CREATE(data))      — direct (same scope)
             *   d) CALL(BOX(CLASS_CREATE(data))) — boxed direct
             * For (c)/(d), also resolve the constructor target. */
            bool is_class_call = false;
            if (callee->op == XI_GET_SHARED) {
                int s = (int)callee->aux_int;
                if (s >= 0 && s < CG_MAX_SHARED && g_shared_class[s])
                    is_class_call = true;
            }
            if (!is_class_call && (callee->op == XI_BOX || callee->op == XI_UNBOX) &&
                callee->nargs >= 1 && callee->args[0]->op == XI_GET_SHARED) {
                int s = (int)callee->args[0]->aux_int;
                if (s >= 0 && s < CG_MAX_SHARED && g_shared_class[s])
                    is_class_call = true;
            }
            /* Direct CLASS_CREATE callee (not via shared slot) */
            if (!is_class_call && callee->op == XI_CLASS_CREATE && callee->aux) {
                const XiClassData *cd = (const XiClassData *)callee->aux;
                const XiFunc *ctor = cg_find_constructor(f, cd);
                if (ctor) {
                    target = ctor;
                    is_class_call = true;
                }
            }
            if (!is_class_call && callee->op == XI_BOX && callee->nargs >= 1 &&
                callee->args[0]->op == XI_CLASS_CREATE && callee->args[0]->aux) {
                const XiClassData *cd = (const XiClassData *)callee->args[0]->aux;
                const XiFunc *ctor = cg_find_constructor(f, cd);
                if (ctor) {
                    target = ctor;
                    is_class_call = true;
                }
            }

            if (target && is_class_call) {
                /* Class constructor call: alloc map instance + call ctor.
                 * xrt_map_new returns a tagged XrValue directly. */
                fprintf(out, "({ XrValue _inst = xrt_map_new(4); ");
                emit_fname(out, prefix, target);
                fprintf(out, "(_inst");
                for (uint16_t a = 1; a < v->nargs; a++) {
                    fprintf(out, ", ");
                    emit_vref(out, v->args[a]);
                }
                fprintf(out, "); _inst; })");
            } else if (target) {
                emit_fname(out, prefix, target);
                fprintf(out, "(");
                for (uint16_t a = 1; a < v->nargs; a++) {
                    if (a > 1) fprintf(out, ", ");
                    emit_vref(out, v->args[a]);
                }
                fprintf(out, ")");
            } else {
                /* Indirect call (fully dynamic, not yet supported) */
                fprintf(out, "XR_NULL_VAL /* TODO: indirect call */");
            }
            break;
        }

        /* Shared variables (module-level) */
        case XI_GET_SHARED:
            fprintf(out, "%s[%d]", g_shared_name, (int)v->aux_int);
            break;

        case XI_SET_SHARED:
            fprintf(out, "(%s[%d] = ", g_shared_name, (int)v->aux_int);
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
            break;

        /* Closure creation — wrap C function pointer in AOT closure value.
         * When the closure is called directly (XI_CALL), the cgen resolves
         * the target and emits a direct C call.  This path handles the case
         * where the closure is passed as an argument (e.g. to .map/.filter). */
        case XI_CLOSURE_NEW:
            if (v->aux) {
                XiFunc *child = (XiFunc *)v->aux;
                fprintf(out, "xrt_closure_new((void*)");
                emit_fname(out, prefix, child);
                fprintf(out, ", 0)");
            } else {
                fprintf(out, "XR_NULL_VAL /* closure: unknown */");
            }
            break;

        /* Upvalue access: reads/writes the file-scope _xrt_upvals array.
         * Non-reentrant but sufficient for defer and simple closures. */
        case XI_LOAD_UPVAL:
            fprintf(out, "_xrt_upvals[%d]", (int)v->aux_int);
            break;

        case XI_STORE_UPVAL:
            fprintf(out, "(_xrt_upvals[%d] = ", (int)v->aux_int);
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
            break;

        /* String concatenation: use xrt_add which handles tagged string values */
        case XI_STR_CONCAT: {
            if (v->nargs == 2) {
                fprintf(out, "xrt_add(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", ");
                emit_vref(out, v->args[1]);
                fprintf(out, ")");
            } else {
                /* Multi-arg: chain pairwise xrt_add(xrt_add(a, b), c) */
                for (uint16_t i = 0; i + 1 < v->nargs; i++)
                    fprintf(out, "xrt_add(");
                emit_vref(out, v->args[0]);
                for (uint16_t i = 1; i < v->nargs; i++) {
                    fprintf(out, ", ");
                    emit_vref(out, v->args[i]);
                    fprintf(out, ")");
                }
            }
            break;
        }

        /* Runtime type check: args[0]=value, aux=target XrType*.
         * AOT tag namespace extends VM tags: string uses XR_TAG_STR(14)
         * and XR_TAG_STR_ARC(19), not XR_TAG_PTR(5). */
        case XI_IS: {
            XR_DCHECK(v->nargs >= 1, "XI_IS: missing arg");
            struct XrType *target = (struct XrType *)v->aux;
            if (!target) {
                fprintf(out, "0 /* XI_IS: NULL target type */");
                break;
            }
            switch (target->kind) {
                case XR_KIND_INT:
                    fprintf(out, "(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ".tag == %u)", XR_TAG_I64);
                    break;
                case XR_KIND_FLOAT:
                    fprintf(out, "(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ".tag == %u)", XR_TAG_F64);
                    break;
                case XR_KIND_BOOL:
                    fprintf(out, "(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ".tag == %u)", XR_TAG_BOOL);
                    break;
                case XR_KIND_NULL:
                    fprintf(out, "(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ".tag == %u)", XR_TAG_NULL);
                    break;
                case XR_KIND_STRING:
                    /* XR_IS_STR checks both XR_TAG_STR and XR_TAG_STR_ARC */
                    fprintf(out, "XR_IS_STR(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ")");
                    break;
                default: {
                    uint8_t tag = xr_type_to_xr_tag(target);
                    if (tag != 0xFF) {
                        fprintf(out, "(");
                        emit_vref(out, v->args[0]);
                        fprintf(out, ".tag == %u)", (unsigned)tag);
                    } else {
                        fprintf(out, "0 /* unsupported is-check */");
                    }
                    break;
                }
            }
            break;
        }

        /* ============ Containers ============ */

        /* Array creation: aux_int = initial capacity hint */
        case XI_ARRAY_NEW: {
            int64_t cap = (v->nargs >= 1 && v->args[0]->op == XI_CONST)
                          ? v->args[0]->aux_int : 4;
            fprintf(out, "xrt_array_new(%" PRId64 ")", cap);
            break;
        }

        /* Map creation: aux_int = initial capacity hint */
        case XI_MAP_NEW: {
            int64_t cap = (v->nargs >= 1 && v->args[0]->op == XI_CONST)
                          ? v->args[0]->aux_int : 8;
            fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
            break;
        }

        /* Set creation (emitted as map with keys-only semantics for now) */
        case XI_SET_NEW: {
            int64_t cap = 8;
            fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
            break;
        }

        /* Indexed read: args[0]=collection, args[1]=key */
        case XI_INDEX_GET:
            XR_DCHECK(v->nargs >= 2, "XI_INDEX_GET: need obj+key");
            fprintf(out, "xrt_index_get(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out, ")");
            break;

        /* Indexed write: args[0]=collection, args[1]=key, args[2]=value */
        case XI_INDEX_SET:
            XR_DCHECK(v->nargs >= 3, "XI_INDEX_SET: need obj+key+val");
            fprintf(out, "xrt_index_set(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out, ", ");
            emit_vref(out, v->args[2]);
            fprintf(out, ")");
            break;

        /* ============ Field Access ============ */

        /* Property read: args[0]=object, aux=field name string */
        case XI_LOAD_FIELD: {
            XR_DCHECK(v->nargs >= 1, "XI_LOAD_FIELD: need object");
            const char *field = (const char *)v->aux;
            /* Use xrt_getprop with symbol lookup for builtin properties,
             * or xrt_map_get for map-like objects */
            int sym = cg_method_sym(field);
            if (sym >= 0) {
                fprintf(out, "xrt_getprop(");
                emit_vref(out, v->args[0]);
                fprintf(out, ", %d)", sym);
            } else {
                /* Generic field: use map get with string key */
                fprintf(out, "xrt_map_get((xrt_map_t*)");
                emit_vref(out, v->args[0]);
                fprintf(out, ".ptr, xr_box_str(\"%s\"))", field ? field : "?");
            }
            break;
        }

        /* Property write: args[0]=object, args[1]=value, aux=field name string */
        case XI_STORE_FIELD: {
            XR_DCHECK(v->nargs >= 2, "XI_STORE_FIELD: need obj+val");
            const char *field = (const char *)v->aux;
            fprintf(out, "(xrt_map_set((xrt_map_t*)");
            emit_vref(out, v->args[0]);
            fprintf(out, ".ptr, xr_box_str(\"%s\"), ", field ? field : "?");
            emit_vref(out, v->args[1]);
            fprintf(out, "), ");
            emit_vref(out, v->args[1]);
            fprintf(out, ")");
            break;
        }

        /* Object allocation: creates a map to hold fields */
        case XI_ALLOC: {
            int64_t cap = (v->nargs >= 1 && v->args[0]->op == XI_CONST)
                          ? v->args[0]->aux_int : 8;
            fprintf(out, "xrt_map_new(%" PRId64 ")", cap);
            break;
        }

        /* ============ Method Call ============ */

        /* Method dispatch: args[0]=recv, args[1..n]=params, aux=name string.
         * Resolution order:
         *   1. Super call (aux_int=1) → find parent class constructor
         *   2. Class instance method → direct C call
         *   3. Builtin method → xrt_method_N runtime dispatch */
        case XI_CALL_METHOD: {
            XR_DCHECK(v->nargs >= 1, "XI_CALL_METHOD: need receiver");
            const char *method = (const char *)v->aux;
            bool is_super = (v->aux_int == 1);
            const XiFunc *mfunc = NULL;

            if (is_super && g_module_func) {
                /* super call: find which class owns the current method,
                 * look up its parent class name, resolve the target.
                 * super() → parent constructor; super.m() → parent method. */
                const char *parent_class = NULL;
                for (uint32_t bi2 = 0; bi2 < g_module_func->nblocks && !parent_class; bi2++) {
                    const XiBlock *blk2 = g_module_func->blocks[bi2];
                    if (!blk2) continue;
                    for (uint32_t vi2 = 0; vi2 < blk2->nvalues; vi2++) {
                        const XiValue *cv = blk2->values[vi2];
                        if (!cv || cv->op != XI_CLASS_CREATE || !cv->aux) continue;
                        const XiClassData *cd = (const XiClassData *)cv->aux;
                        if (!cd->ast) continue;
                        const ClassDeclNode *cls = &cd->ast->as.class_decl;
                        if (!cls->super_name) continue;
                        for (uint16_t ci = 0; ci < cd->ninst + cd->nstat; ci++) {
                            if (cd->child_idx && cd->child_idx[ci] < g_module_func->nchildren &&
                                g_module_func->children[cd->child_idx[ci]] == f) {
                                parent_class = cls->super_name;
                                break;
                            }
                        }
                        if (parent_class) break;
                    }
                }
                if (parent_class) {
                    bool is_ctor_call = (method && strcmp(method, "constructor") == 0);
                    if (is_ctor_call)
                        mfunc = cg_lookup_class_ctor(g_module_func, parent_class);
                    else
                        mfunc = cg_lookup_method(method, parent_class);
                }
            }
            if (!mfunc && !is_super) {
                /* Try receiver-type-specific lookup first.
                 * XI_CALL_METHOD args[0] = receiver, whose type may carry
                 * the class name (XR_KIND_INSTANCE → instance.class_name). */
                const char *recv_class = NULL;
                if (v->args[0] && v->args[0]->type &&
                    v->args[0]->type->kind == XR_KIND_INSTANCE)
                    recv_class = v->args[0]->type->instance.class_name;
                mfunc = cg_lookup_method(method, recv_class);
            }
            uint16_t nargs = (uint16_t)(v->nargs - 1);

            if (mfunc) {
                /* Direct class method call: receiver is first param */
                emit_fname(out, prefix, mfunc);
                fprintf(out, "(");
                for (uint16_t a = 0; a < v->nargs; a++) {
                    if (a > 0) fprintf(out, ", ");
                    emit_vref(out, v->args[a]);
                }
                fprintf(out, ")");
            } else {
                int sym = cg_method_sym(method);
                if (nargs == 0) {
                    fprintf(out, "xrt_method_0(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", %d)", sym >= 0 ? sym : 0);
                } else if (nargs == 1) {
                    fprintf(out, "xrt_method_1(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", %d, ", sym >= 0 ? sym : 0);
                    emit_vref(out, v->args[1]);
                    fprintf(out, ")");
                } else if (nargs == 2) {
                    fprintf(out, "xrt_method_2(");
                    emit_vref(out, v->args[0]);
                    fprintf(out, ", %d, ", sym >= 0 ? sym : 0);
                    emit_vref(out, v->args[1]);
                    fprintf(out, ", ");
                    emit_vref(out, v->args[2]);
                    fprintf(out, ")");
                } else {
                    fprintf(out, "XR_NULL_VAL /* TODO: method %d args */", nargs);
                }
            }
            break;
        }

        /* ============ Type Operations ============ */

        /* typeof(x): aux_int=0 → integer type ID, aux_int=1 → type name string */
        case XI_TYPEOF: {
            XR_DCHECK(v->nargs >= 1, "XI_TYPEOF: need arg");
            if (v->aux_int == 1) {
                /* typename(): return type name as boxed string */
                fprintf(out, "xrt_typeof_str(");
                emit_vref(out, v->args[0]);
                fprintf(out, ")");
            } else {
                /* typeof(): return integer type ID matching VM semantics.
                 * Result is always TAGGED (XrValue) so wrap in XR_FROM_INT. */
                fprintf(out, "XR_FROM_INT(xrt_typeof_id(");
                emit_vref(out, v->args[0]);
                fprintf(out, "))");
            }
            break;
        }

        /* throw(value): abort with exception */
        case XI_THROW:
            XR_DCHECK(v->nargs >= 1, "XI_THROW: need arg");
            fprintf(out, "xrt_throw_exc(");
            emit_vref(out, v->args[0]);
            fprintf(out, ")");
            break;

        /* ============ Assertions ============ */

        /* assert(cond): aux=location string, aux_int: 0=truthy, 1=falsy */
        case XI_ASSERT: {
            XR_DCHECK(v->nargs >= 1, "XI_ASSERT: need cond");
            const char *loc = v->aux ? (const char *)v->aux : "<unknown>";
            bool invert = (v->aux_int == 1);
            if (invert) {
                fprintf(out, "(xr_truthy(");
                emit_vref(out, v->args[0]);
                fprintf(out, ") ? (fprintf(stderr, \"Assertion failed (expected false): %s\\n\"), abort(), XR_NULL_VAL) : XR_NULL_VAL)",
                        loc);
            } else {
                fprintf(out, "(!xr_truthy(");
                emit_vref(out, v->args[0]);
                fprintf(out, ") ? (fprintf(stderr, \"Assertion failed: %s\\n\"), abort(), XR_NULL_VAL) : XR_NULL_VAL)",
                        loc);
            }
            break;
        }

        /* assert_eq(actual, expected): aux=location string */
        case XI_ASSERT_EQ: {
            XR_DCHECK(v->nargs >= 2, "XI_ASSERT_EQ: need 2 args");
            const char *loc = v->aux ? (const char *)v->aux : "<unknown>";
            fprintf(out, "(xrt_eq(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out, ") ? XR_NULL_VAL : (fprintf(stderr, \"assert_eq failed: %s\\n\"), abort(), XR_NULL_VAL))",
                    loc);
            break;
        }

        /* assert_ne(actual, unexpected): aux=location string */
        case XI_ASSERT_NE: {
            XR_DCHECK(v->nargs >= 2, "XI_ASSERT_NE: need 2 args");
            const char *loc = v->aux ? (const char *)v->aux : "<unknown>";
            fprintf(out, "(!xrt_eq(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", ");
            emit_vref(out, v->args[1]);
            fprintf(out, ") ? XR_NULL_VAL : (fprintf(stderr, \"assert_ne failed: %s\\n\"), abort(), XR_NULL_VAL))",
                    loc);
            break;
        }

        /* ============ Iteration ============ */

        /* iter_new(coll) → xrt_method_0(coll, SYMBOL_ITERATOR) */
        case XI_ITER_NEW:
            XR_DCHECK(v->nargs >= 1, "XI_ITER_NEW: need collection");
            fprintf(out, "xrt_method_0(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", %d)", XRT_SYM_ITERATOR);
            break;

        /* iter_valid(iter) → xrt_method_0(iter, SYMBOL_HAS_NEXT) */
        case XI_ITER_VALID:
            XR_DCHECK(v->nargs >= 1, "XI_ITER_VALID: need iterator");
            fprintf(out, "xr_truthy(xrt_method_0(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", %d))", XRT_SYM_HAS_NEXT);
            break;

        /* iter_next(iter) → xrt_method_0(iter, SYMBOL_NEXT) */
        case XI_ITER_NEXT:
            XR_DCHECK(v->nargs >= 1, "XI_ITER_NEXT: need iterator");
            fprintf(out, "xrt_method_0(");
            emit_vref(out, v->args[0]);
            fprintf(out, ", %d)", XRT_SYM_NEXT);
            break;

        /* ============ Exception Handling ============ */

        /* TRY/END_TRY/FINALLY: handled structurally in emit_value_stmt */
        case XI_TRY:
        case XI_END_TRY:
        case XI_FINALLY:
            fprintf(out, "XR_NULL_VAL");
            break;

        /* CATCH: destination receives caught exception from the frame.
         * Find the matching XI_TRY to use the correct _efN. */
        case XI_CATCH: {
            uint32_t try_id = 0;
            for (uint32_t bi = 0; bi < f->nblocks; bi++) {
                const XiBlock *blk2 = f->blocks[bi];
                if (!blk2) continue;
                for (uint32_t vi2 = 0; vi2 < blk2->nvalues; vi2++) {
                    const XiValue *tv = blk2->values[vi2];
                    if (tv && tv->op == XI_TRY) try_id = tv->id;
                }
            }
            fprintf(out, "_ef%u.exception", try_id);
            break;
        }

        /* Defer: no-op in emit_value_rhs — actual calls are emitted
         * before RETURN terminators in emit_block(). */
        case XI_DEFER:
            fprintf(out, "XR_NULL_VAL");
            break;

        /* Builtin calls */
        case XI_CALL_BUILTIN:
            fprintf(out, "XR_NULL_VAL /* TODO: builtin '%s' */",
                    v->aux ? (const char *)v->aux : "?");
            break;

        /* Class creation: the value itself is not used at runtime in AOT.
         * Constructor calls go through the XI_CALL class_slot path which
         * allocates instances and calls the constructor directly.
         * Emit NULL_VAL so the SSA value has a valid C definition. */
        case XI_CLASS_CREATE:
            fprintf(out, "XR_NULL_VAL /* class descriptor */");
            break;

        default:
            fprintf(out, "XR_NULL_VAL /* TODO: op %d */", v->op);
            break;
    }
}

/* Emit a complete value statement: type vN = <rhs>; */
static void emit_value_stmt(FILE *out, const XiFunc *f, const XiValue *v,
                              const char *prefix) {
    XR_DCHECK(v != NULL, "emit_value_stmt: NULL value");

    /* Side-effect-only values that don't produce a named result. */
    bool void_like = (v->op == XI_PRINT || v->op == XI_SET_SHARED ||
                      v->op == XI_STORE_UPVAL || v->op == XI_STORE_FIELD ||
                      v->op == XI_INDEX_SET || v->op == XI_THROW);

    if (void_like) {
        fprintf(out, "    ");
        emit_value_rhs(out, f, v, prefix);
        fprintf(out, ";\n");
        return;
    }

    /* XI_TRY: emit setjmp/longjmp exception frame setup.
     * The exception frame must be declared at function scope so it
     * survives across goto labels.  Use the value ID for uniqueness. */
    if (v->op == XI_TRY) {
        const XiBlock *catch_blk = (const XiBlock *)v->aux;
        fprintf(out, "    XrtExcFrame _ef%u;\n", v->id);
        fprintf(out, "    _ef%u.prev = xrt_exc_top;\n", v->id);
        fprintf(out, "    xrt_exc_top = &_ef%u;\n", v->id);
        if (catch_blk) {
            fprintf(out, "    if (setjmp(_ef%u.buf) != 0) {"
                         " xrt_exc_top = _ef%u.prev; goto L%u; }\n",
                         v->id, v->id, catch_blk->id);
        } else {
            fprintf(out, "    if (setjmp(_ef%u.buf) != 0) {"
                         " xrt_exc_top = _ef%u.prev; }\n",
                         v->id, v->id);
        }
        return;
    }

    /* XI_END_TRY / XI_FINALLY: pop the exception frame.
     * Finds the matching XI_TRY by scanning earlier blocks. */
    if (v->op == XI_END_TRY || v->op == XI_FINALLY) {
        /* Find the TRY value ID for the matching _efN variable */
        uint32_t try_id = 0;
        for (uint32_t bi = 0; bi < f->nblocks; bi++) {
            const XiBlock *blk = f->blocks[bi];
            if (!blk) continue;
            for (uint32_t vi2 = 0; vi2 < blk->nvalues; vi2++) {
                const XiValue *tv = blk->values[vi2];
                if (tv && tv->op == XI_TRY) try_id = tv->id;
            }
        }
        fprintf(out, "    xrt_exc_top = _ef%u.prev;\n", try_id);
        return;
    }

    if (g_pre_decl_all) {
        /* Variable already declared at function top — emit assignment */
        fprintf(out, "    ");
        emit_vref(out, v);
        fprintf(out, " = ");
        emit_value_rhs(out, f, v, prefix);
        fprintf(out, ";\n");
    } else {
        XrRep rep = cg_rep(v);
        fprintf(out, "    %s ", ctype_str(rep));
        emit_vref(out, v);
        fprintf(out, " = ");
        emit_value_rhs(out, f, v, prefix);
        fprintf(out, ";\n");
    }
}

/* ========== PHI Elimination ========== */

/* Emit phi assignments for all phis in `target` whose predecessor
 * at index `pred_idx` is `pred_blk`. Called before the jump/branch. */
static void emit_phi_copies(FILE *out, const XiBlock *target,
                              uint16_t pred_idx) {
    for (const XiPhi *phi = target->phis; phi; phi = phi->next) {
        if (pred_idx < phi->value.nargs && phi->value.args[pred_idx]) {
            fprintf(out, "    ");
            emit_phi_ref(out, phi);
            fprintf(out, " = ");
            emit_vref(out, phi->value.args[pred_idx]);
            fprintf(out, ";\n");
        }
    }
}

/* Find predecessor index of `pred` in `blk`. */
static uint16_t find_pred_idx(const XiBlock *blk, const XiBlock *pred) {
    for (uint16_t i = 0; i < blk->npreds; i++) {
        if (blk->preds[i] == pred) return i;
    }
    return 0; /* fallback; should not happen in valid IR */
}

/* ========== Block Emission ========== */

static void emit_block(FILE *out, const XiFunc *f, const XiBlock *blk,
                         const char *prefix) {
    XR_DCHECK(blk != NULL, "emit_block: NULL block");

    /* Label (skip for entry block b0 to reduce clutter) */
    if (blk->id != 0)
        fprintf(out, "L%u:;\n", blk->id);

    /* Instructions */
    for (uint32_t i = 0; i < blk->nvalues; i++) {
        XiValue *v = blk->values[i];
        if (!v) continue;
        emit_value_stmt(out, f, v, prefix);
    }

    /* Terminator */
    switch (blk->kind) {
        case XI_BLOCK_RETURN: {
            /* Call deferred closures in LIFO order before returning.
             * Scan all blocks for XI_DEFER whose args[0] is CLOSURE_NEW. */
            const XiFunc *deferred[32];
            int ndeferred = 0;
            for (uint32_t dbi = 0; dbi < f->nblocks && ndeferred < 32; dbi++) {
                const XiBlock *db = f->blocks[dbi];
                if (!db) continue;
                for (uint32_t dvi = 0; dvi < db->nvalues; dvi++) {
                    const XiValue *dv = db->values[dvi];
                    if (!dv || dv->op != XI_DEFER || dv->nargs < 1) continue;
                    const XiValue *callee = dv->args[0];
                    if (callee && callee->op == XI_CLOSURE_NEW && callee->aux)
                        deferred[ndeferred++] = (const XiFunc *)callee->aux;
                }
            }
            for (int di = ndeferred - 1; di >= 0; di--) {
                fprintf(out, "    ");
                emit_fname(out, prefix, deferred[di]);
                fprintf(out, "();\n");
            }
            if (blk->control) {
                fprintf(out, "    return ");
                emit_vref(out, blk->control);
                fprintf(out, ";\n");
            } else {
                fprintf(out, "    return XR_NULL_VAL;\n");
            }
            break;
        }

        case XI_BLOCK_PLAIN:
            if (blk->succs[0]) {
                emit_phi_copies(out, blk->succs[0],
                                find_pred_idx(blk->succs[0], blk));
                fprintf(out, "    goto L%u;\n", blk->succs[0]->id);
            }
            break;

        case XI_BLOCK_IF:
            XR_DCHECK(blk->control != NULL, "IF block missing control");
            XR_DCHECK(blk->succs[0] != NULL, "IF block missing then");
            XR_DCHECK(blk->succs[1] != NULL, "IF block missing else");
            /* Emit phi copies for both branches */
            fprintf(out, "    if (");
            emit_vref(out, blk->control);
            fprintf(out, ") {\n");
            emit_phi_copies(out, blk->succs[0],
                            find_pred_idx(blk->succs[0], blk));
            fprintf(out, "        goto L%u;\n", blk->succs[0]->id);
            fprintf(out, "    } else {\n");
            emit_phi_copies(out, blk->succs[1],
                            find_pred_idx(blk->succs[1], blk));
            fprintf(out, "        goto L%u;\n", blk->succs[1]->id);
            fprintf(out, "    }\n");
            break;

        case XI_BLOCK_UNREACHABLE:
            fprintf(out, "    __builtin_unreachable();\n");
            break;

        default:
            fprintf(out, "    /* unknown block kind %d */\n", blk->kind);
            break;
    }
}

/* ========== Function Emission ========== */

/* Check whether a function contains any exception handling ops.
 * If so, all variables must be pre-declared at function scope to
 * avoid jumping over declarations with initializers via goto. */
static bool cg_has_exception_handling(const XiFunc *f) {
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk) continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            if (blk->values[vi] && blk->values[vi]->op == XI_TRY)
                return true;
        }
    }
    return false;
}

/* Collect all values and phis to declare at function top.
 * When the function contains exception handling (setjmp/goto),
 * ALL SSA values are pre-declared to avoid jumping over decls. */
static void emit_declarations(FILE *out, const XiFunc *f) {
    bool pre_decl_all = cg_has_exception_handling(f);

    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk) continue;

        /* Phi variables (always pre-declared) */
        for (const XiPhi *phi = blk->phis; phi; phi = phi->next) {
            XrRep rep = cg_rep(&phi->value);
            fprintf(out, "    %s ", ctype_str(rep));
            emit_phi_ref(out, phi);
            if (rep == XR_REP_TAGGED)
                fprintf(out, " = XR_NULL_VAL;\n");
            else
                fprintf(out, " = 0;\n");
        }

        /* SSA values (only pre-declared when exception handling present) */
        if (pre_decl_all) {
            for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
                const XiValue *v = blk->values[vi];
                if (!v) continue;
                /* Skip void-like ops and structural exception ops */
                if (v->op == XI_PRINT || v->op == XI_SET_SHARED ||
                    v->op == XI_STORE_UPVAL || v->op == XI_STORE_FIELD ||
                    v->op == XI_INDEX_SET || v->op == XI_THROW ||
                    v->op == XI_TRY || v->op == XI_END_TRY ||
                    v->op == XI_FINALLY)
                    continue;
                XrRep rep = cg_rep(v);
                fprintf(out, "    %s ", ctype_str(rep));
                emit_vref(out, v);
                if (rep == XR_REP_TAGGED)
                    fprintf(out, " = XR_NULL_VAL;\n");
                else
                    fprintf(out, " = 0;\n");
            }
        }
    }
}

XR_FUNC void xi_cgen_func(FILE *out, XiFunc *f, const char *prefix) {
    XR_DCHECK(out != NULL, "xi_cgen_func: NULL output");
    XR_DCHECK(f != NULL, "xi_cgen_func: NULL func");

    /* Emit nested children first (forward declarations already emitted) */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            xi_cgen_func(out, f->children[i], prefix);
    }

    /* Function signature */
    fprintf(out, "static XrValue ");
    emit_fname(out, prefix, f);
    fprintf(out, "(");
    if (f->nparams == 0) {
        fprintf(out, "void");
    } else {
        for (uint16_t i = 0; i < f->nparams; i++) {
            if (i > 0) fprintf(out, ", ");
            fprintf(out, "XrValue p%u", i);
        }
    }
    fprintf(out, ") {\n");

    /* Shared array declared at file scope by xi_cgen_program(),
     * so child functions can reference it too. */

    /* Pre-declare variables (phis always; all SSA values when
     * exception handling is present to avoid goto-over-decl UB). */
    g_pre_decl_all = cg_has_exception_handling(f);
    emit_declarations(out, f);

    /* Blocks in order */
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        if (f->blocks[bi])
            emit_block(out, f, f->blocks[bi], prefix);
    }

    fprintf(out, "}\n\n");
}

/* ========== Forward Declarations ========== */

static void emit_forward_decls(FILE *out, const XiFunc *f,
                                 const char *prefix) {
    /* Recurse children first */
    for (uint16_t i = 0; i < f->nchildren; i++) {
        if (f->children[i])
            emit_forward_decls(out, f->children[i], prefix);
    }

    fprintf(out, "static XrValue ");
    emit_fname(out, prefix, f);
    fprintf(out, "(");
    if (f->nparams == 0) {
        fprintf(out, "void");
    } else {
        for (uint16_t i = 0; i < f->nparams; i++) {
            if (i > 0) fprintf(out, ", ");
            fprintf(out, "XrValue p%u", i);
        }
    }
    fprintf(out, ");\n");
}

/* ========== Multi-module API ========== */

XR_FUNC void xi_cgen_header(FILE *out) {
    XR_DCHECK(out != NULL, "xi_cgen_header: NULL output");
    fprintf(out, "/* Generated by xi_cgen \xe2\x80\x94 do not edit */\n");
    fprintf(out, "#define XRT_IMPL\n");
    fprintf(out, "#include \"xrt.h\"\n\n");
}

/* Find the maximum upvalue index referenced across a function and all
 * its children.  Returns -1 if no upvalues are used. */
static int cg_max_upval_index(const XiFunc *f) {
    int mx = -1;
    for (uint32_t bi = 0; bi < f->nblocks; bi++) {
        const XiBlock *blk = f->blocks[bi];
        if (!blk) continue;
        for (uint32_t vi = 0; vi < blk->nvalues; vi++) {
            const XiValue *v = blk->values[vi];
            if (!v) continue;
            if (v->op == XI_LOAD_UPVAL || v->op == XI_STORE_UPVAL) {
                int idx = (int)v->aux_int;
                if (idx > mx) mx = idx;
            }
        }
    }
    for (uint16_t ci = 0; ci < f->nchildren; ci++) {
        if (!f->children[ci]) continue;
        int child_mx = cg_max_upval_index(f->children[ci]);
        if (child_mx > mx) mx = child_mx;
    }
    return mx;
}

XR_FUNC void xi_cgen_module(FILE *out, XiFunc *module_func,
                             const char *module_name) {
    XR_DCHECK(out != NULL, "xi_cgen_module: NULL output");
    XR_DCHECK(module_func != NULL, "xi_cgen_module: NULL func");

    const char *prefix = module_name ? module_name : "mod";

    /* Prescan to build shared-slot → XiFunc mapping */
    cg_prescan_shared(module_func);

    /* Module-scoped shared variable array.
     * Multi-module builds use a prefixed name to avoid collisions. */
    char shared_buf[256];
    snprintf(shared_buf, sizeof(shared_buf), "xrt_shared_%s", prefix);
    g_shared_name = shared_buf;

    if (module_func->nshared > 0)
        fprintf(out, "static XrValue %s[%u];\n", g_shared_name, module_func->nshared);

    int max_upval = cg_max_upval_index(module_func);
    if (max_upval >= 0)
        fprintf(out, "static XrValue _xrt_upvals[%d];\n", max_upval + 1);
    fprintf(out, "\n");

    /* Forward declarations */
    emit_forward_decls(out, module_func, prefix);
    fprintf(out, "\n");

    /* Function bodies */
    xi_cgen_func(out, module_func, prefix);

    /* Reset shared name to default */
    g_shared_name = "xrt_shared";
}

XR_FUNC void xi_cgen_main(FILE *out, const char **module_names,
                           XiFunc **module_funcs, int n, int entry_index) {
    XR_DCHECK(out != NULL, "xi_cgen_main: NULL output");
    XR_DCHECK(n > 0, "xi_cgen_main: no modules");
    XR_DCHECK(entry_index >= 0 && entry_index < n, "xi_cgen_main: bad entry_index");

    fprintf(out, "int main(void) {\n");
    /* Call each module init in topo order (entry module last) */
    for (int m = 0; m < n; m++) {
        if (!module_funcs[m]) continue;
        fprintf(out, "    ");
        emit_fname(out, module_names[m], module_funcs[m]);
        fprintf(out, "();\n");
    }
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
}

XR_FUNC void xi_cgen_program(FILE *out, XiFunc *main_func,
                               const char *module_name) {
    XR_DCHECK(out != NULL, "xi_cgen_program: NULL output");
    XR_DCHECK(main_func != NULL, "xi_cgen_program: NULL main_func");

    const char *prefix = module_name ? module_name : "mod";

    /* Reset global function name counter for each compilation unit */
    g_fname_counter = 0;

    /* Prescan shared slots for call resolution */
    cg_prescan_shared(main_func);

    /* Single-module uses the simple name */
    g_shared_name = "xrt_shared";

    /* Header */
    xi_cgen_header(out);

    /* File-scope shared variable array */
    if (main_func->nshared > 0)
        fprintf(out, "static XrValue xrt_shared[%u];\n", main_func->nshared);

    /* File-scope upvalue array for closure/defer capture.
     * Scan all children to find max upvalue index used. */
    int max_upval = cg_max_upval_index(main_func);
    if (max_upval >= 0)
        fprintf(out, "static XrValue _xrt_upvals[%d];\n", max_upval + 1);
    fprintf(out, "\n");

    /* Forward declarations */
    emit_forward_decls(out, main_func, prefix);
    fprintf(out, "\n");

    /* Function bodies */
    xi_cgen_func(out, main_func, prefix);

    /* Entry point */
    fprintf(out, "int main(void) {\n");
    fprintf(out, "    ");
    emit_fname(out, prefix, main_func);
    fprintf(out, "();\n");
    fprintf(out, "    return 0;\n");
    fprintf(out, "}\n");
}
