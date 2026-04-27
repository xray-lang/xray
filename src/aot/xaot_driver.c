/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xaot_driver.c - AOT native compilation driver
 *
 * KEY CONCEPT:
 *   Full pipeline from source file to generated C program:
 *   1. Bundle discovery (topo-sorted module list)
 *   2. Per-module: compile → XrProto, scan bytecode for shared_protos /
 *      exports / class metadata
 *   3. Cross-module export map for import resolution
 *   4. XIR lowering + optimization + C emission per function
 *   5. Main() generation calling module inits in topo order
 *
 *   All bytecode pattern-matching lives here so the CLI stays thin.
 *
 * RELATED MODULES:
 *   - xcgen.h: per-function XIR → C lowering
 *   - xaot_driver.h: public API
 *   - xcmd_build.c: CLI entry that invokes xaot_build + CC
 */

#include "xaot_driver.h"
#include "xcgen.h"
#include "../../include/xray.h"
#include "../../include/xray_isolate.h"
#include "../runtime/xisolate_api.h"
#include "../module/xbundle.h"
#include "../runtime/value/xchunk.h"
#include "../runtime/value/xslot_type.h"
#include "../runtime/value/xtype.h"
#include "../runtime/object/xstring.h"
#include "../base/xdynarray.h"
#include "../base/xmalloc.h"
#include "../jit/xir_builder.h"
#include "../jit/xir_pass.h"
#include "../runtime/class/xclass_descriptor.h"
#include "../runtime/class/xclass.h"
#include "../runtime/class/xclass_lookup.h"
#include "../runtime/closure/xclosure.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

/* ========== File reading helper (avoids CLI layer dependency) ========== */

/* Create a full-runtime isolate for AOT compilation.
 * Equivalent to XR_CLI_ISOLATE_RUN profile without CLI dependency. */
static XrayIsolate *create_isolate(void) {
    XrayIsolateParams params;
    xray_isolate_params_init(&params);
    xray_isolate_setup_full(&params);
    return xray_isolate_new(&params);
}

static char *read_source_file(const char *path) {
    XR_DCHECK(path != NULL, "read_source_file: NULL path");
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    char *buf = (char *) xr_malloc((size_t) sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t) sz, f);
    fclose(f);
    buf[nread] = '\0';
    return buf;
}

/* ========== Per-module state during AOT compilation ========== */

typedef struct {
    const char *path;          /* absolute source path (owned) */
    const char *mod_name;      /* C-safe module name (owned) */
    XrProto *proto;            /* compiled bytecode (owned by isolate) */
    XcgenModule *cmod;         /* codegen module (owned by comp) */
    XrProto **aot_protos;      /* AOT-eligible protos (owned) */
    char (*aot_c_names)[140];  /* C function names (owned) */
    int aot_count;
    int aot_cap;
    XirAotExportSlot *export_slots;  /* synthetic export slots (owned) */
    int export_slot_count;
} XaotModuleInfo;

/* ========== Bytecode Scanning Helpers ========== */

/* Build shared_index → child_proto mapping by scanning top-level bytecode.
 * Recognizes CLOSURE+SETSHARED and CLASS_FROM_DESC+SETSHARED patterns.
 * Returns max shared index + 1 (size of the populated region). */
static int build_shared_proto_map(XrProto *top, XrProto **shared_protos, bool *shared_is_ctor,
                                  int max_shared) {
    if (!top)
        return 0;
    int max_idx = 0;
    uint32_t code_count = (uint32_t) top->code.count;
    const XrInstruction *code = (const XrInstruction *) top->code.data;
    for (uint32_t pc = 0; pc + 1 < code_count; pc++) {
        XrInstruction inst0 = code[pc];
        XrInstruction inst1 = code[pc + 1];
        OpCode op0 = GET_OPCODE(inst0);
        OpCode op1 = GET_OPCODE(inst1);

        /* Pattern 1: CLOSURE R[A]; SETSHARED R[A] */
        if (op0 == OP_CLOSURE && op1 == OP_SETSHARED) {
            int closure_dst = GETARG_A(inst0);
            int setshared_src = GETARG_A(inst1);
            if (closure_dst == setshared_src) {
                uint16_t proto_idx = GETARG_Bx(inst0);
                int shared_bx = GETARG_Bx(inst1);
                int abs_idx = shared_bx + top->shared_offset;
                if (abs_idx >= 0 && abs_idx < max_shared && proto_idx < PROTO_PROTO_COUNT(top)) {
                    shared_protos[abs_idx] = PROTO_PROTO(top, proto_idx);
                    if (abs_idx + 1 > max_idx)
                        max_idx = abs_idx + 1;
                }
            }
        }

        /* Pattern 2: CLASS_FROM_DESC R[X]; [MOVE;] SETSHARED R[X] */
        if (op0 == OP_CLASS_CREATE_FROM_DESCRIPTOR) {
            int class_reg = GETARG_A(inst0);
            int ss_pc = -1;
            if (op1 == OP_SETSHARED && GETARG_A(inst1) == class_reg) {
                ss_pc = (int) pc + 1;
            } else if (op1 == OP_MOVE && pc + 2 < code_count) {
                XrInstruction inst2 = code[pc + 2];
                if (GET_OPCODE(inst2) == OP_SETSHARED && GETARG_A(inst2) == class_reg) {
                    ss_pc = (int) pc + 2;
                }
            }
            if (ss_pc >= 0) {
                int shared_bx = GETARG_Bx(code[ss_pc]);
                int abs_idx = shared_bx + top->shared_offset;
                /* Scan backward for first CLOSURE targeting class_reg */
                int ctor_pc = -1;
                for (int scan = (int) pc - 1; scan >= 0; scan--) {
                    OpCode sop = GET_OPCODE(code[scan]);
                    if (sop == OP_CLOSURE && GETARG_A(code[scan]) == class_reg)
                        ctor_pc = scan;  /* keep updating — earliest wins */
                    else if (ctor_pc >= 0)
                        break;
                }
                if (ctor_pc >= 0 && abs_idx >= 0 && abs_idx < max_shared) {
                    uint16_t ctor_proto_idx = GETARG_Bx(code[ctor_pc]);
                    if (ctor_proto_idx < PROTO_PROTO_COUNT(top)) {
                        shared_protos[abs_idx] = PROTO_PROTO(top, ctor_proto_idx);
                        shared_is_ctor[abs_idx] = true;
                        if (abs_idx + 1 > max_idx)
                            max_idx = abs_idx + 1;
                    }
                }
            }
        }
    }
    return max_idx;
}

/* Pre-register classes by scanning top-level bytecode for CLASS_FROM_DESC.
 * Creates minimal class objects so xr_class_lookup_by_name works at AOT
 * compile time.  Also patches method protos with 'this' type annotations
 * and registers class metadata into comp for type table codegen. */
static void preregister_classes(XrProto *proto, XrayIsolate *isolate, XcgenCompilation *comp) {
    if (!proto || !isolate)
        return;
    uint32_t code_count = (uint32_t) proto->code.count;
    const XrInstruction *code = (const XrInstruction *) proto->code.data;

    XrClass *reg_class[256];
    memset(reg_class, 0, sizeof(reg_class));

    for (uint32_t pc = 0; pc < code_count; pc++) {
        XrInstruction inst = code[pc];
        OpCode op = GET_OPCODE(inst);

        if (op == OP_MOVE) {
            int dst = GETARG_A(inst);
            int src = GETARG_B(inst);
            if (dst < 256 && src < 256)
                reg_class[dst] = reg_class[src];
            continue;
        }

        if (op != OP_CLASS_CREATE_FROM_DESCRIPTOR)
            continue;
        int a = GETARG_A(inst);
        int bx = GETARG_Bx(inst);
        if (bx >= (int) PROTO_CONST_COUNT(proto))
            continue;
        XrValue desc_val = PROTO_CONSTANT(proto, bx);
        XrClassDescriptor *desc = (XrClassDescriptor *) XR_TO_PTR(desc_val);
        if (!desc)
            continue;

        XrClass *super_override = (a < 256) ? reg_class[a] : NULL;
        XrClass *klass =
            xr_class_from_descriptor(isolate, desc, proto, NULL, NULL, NULL, super_override);
        if (!klass)
            continue;

        if (a < 256)
            reg_class[a] = klass;

        /* Register class info for AOT type table codegen */
        if (comp) {
            XrProto *ctor_proto = NULL;
            for (int scan = (int) pc - 1; scan >= 0; scan--) {
                OpCode sop = GET_OPCODE(code[scan]);
                if (sop == OP_CLOSURE && GETARG_A(code[scan]) == a) {
                    uint16_t pidx = GETARG_Bx(code[scan]);
                    if (pidx < PROTO_PROTO_COUNT(proto)) {
                        XrProto *cp = PROTO_PROTO(proto, pidx);
                        if (cp && cp->name && strcmp(XR_STRING_CHARS(cp->name), "constructor") == 0)
                            ctor_proto = cp;
                    }
                } else if (ctor_proto) {
                    break;
                }
            }
            if (ctor_proto) {
                const char *parent_name = desc->super_name;
                if (!parent_name && super_override)
                    parent_name = super_override->name;
                xcgen_register_class(comp, (void *) ctor_proto, desc->class_name, parent_name,
                                     (int) desc->instance_field_count);
            }
        }

        /* Patch method protos: set 'this' (param 0) type */
        for (uint32_t mi = 0; mi < desc->instance_method_count && mi < klass->method_count; mi++) {
            XrMethod *method = &klass->methods[mi];
            if (method->type != XMETHOD_CLOSURE || !method->as.closure)
                continue;
            XrProto *mp = method->as.closure->proto;
            if (!mp || mp->numparams < 1 || mp->numparams > 200)
                continue;
            if (!mp->param_types) {
                mp->param_types =
                    (struct XrType **) xr_calloc((size_t) mp->numparams, sizeof(struct XrType *));
                if (!mp->param_types)
                    continue;
                mp->param_types_count = (uint8_t) mp->numparams;
            }
            if (mp->param_types_count > 0 && !mp->param_types[0]) {
                mp->param_types[0] = xr_type_new_named_instance(isolate, desc->class_name);
            }
        }
    }
}

/* Scan bytecodes for OP_EXPORT and correlate with OP_SETSHARED to
 * populate exports in the codegen module.  For const exports without
 * SETSHARED, allocates synthetic shared indices and populates
 * out_slots/out_nslots so the XIR builder can emit SETSHARED. */
static void collect_exports(XrProto *proto, XcgenModule *mod, XirAotExportSlot **out_slots,
                            int *out_nslots) {
    if (out_slots)
        *out_slots = NULL;
    if (out_nslots)
        *out_nslots = 0;
    if (!proto || !mod)
        return;
    uint32_t code_count = (uint32_t) proto->code.count;
    const XrInstruction *code = (const XrInstruction *) proto->code.data;

    int max_local_bx = -1;
    for (uint32_t pc = 0; pc < code_count; pc++) {
        if (GET_OPCODE(code[pc]) == OP_SETSHARED) {
            int bx = GETARG_Bx(code[pc]);
            if (bx > max_local_bx)
                max_local_bx = bx;
        }
    }
    int next_local_bx = max_local_bx + 1;

    XirAotExportSlot synth[64];
    int nsynth = 0;

    for (uint32_t pc = 0; pc < code_count; pc++) {
        XrInstruction inst = code[pc];
        if (GET_OPCODE(inst) != OP_EXPORT)
            continue;

        int name_idx = GETARG_A(inst);
        int value_reg = GETARG_B(inst);
        int is_const = GETARG_C(inst);

        if (name_idx >= (int) VALUEARRAY_COUNT(&proto->constants))
            continue;
        XrValue name_val = VALUEARRAY_GET(&proto->constants, name_idx);
        if (!XR_IS_STRING(name_val))
            continue;
        const char *export_name = XR_STRING_CHARS(XR_TO_STRING(name_val));

        /* Trace backwards through MOVE chain to find GETSHARED/SETSHARED */
        int shared_idx = -1;
        int chain[16];
        int chain_len = 0;
        chain[chain_len++] = value_reg;
        for (int bpc = (int) pc - 1; bpc >= 0 && chain_len < 16; bpc--) {
            XrInstruction prev = code[bpc];
            if (GET_OPCODE(prev) == OP_MOVE && GETARG_A(prev) == chain[chain_len - 1]) {
                chain[chain_len++] = GETARG_B(prev);
            }
        }
        for (int bpc = (int) pc - 1; bpc >= 0 && shared_idx < 0; bpc--) {
            XrInstruction prev = code[bpc];
            OpCode prev_op = GET_OPCODE(prev);
            if (prev_op == OP_GETSHARED || prev_op == OP_SETSHARED) {
                int reg_a = GETARG_A(prev);
                for (int ci = 0; ci < chain_len; ci++) {
                    if (chain[ci] == reg_a) {
                        shared_idx = GETARG_Bx(prev) + proto->shared_offset;
                        break;
                    }
                }
            }
        }

        if (shared_idx < 0) {
            shared_idx = next_local_bx + proto->shared_offset;
            if (nsynth < 64) {
                synth[nsynth].export_pc = pc;
                synth[nsynth].value_reg = value_reg;
                synth[nsynth].shared_index = shared_idx;
                nsynth++;
            }
            next_local_bx++;
        }
        xcgen_module_add_export(mod, export_name, shared_idx, is_const != 0);
    }

    if (nsynth > 0 && out_slots && out_nslots) {
        *out_slots = (XirAotExportSlot *) xr_malloc(nsynth * sizeof(XirAotExportSlot));
        if (*out_slots) {
            memcpy(*out_slots, synth, nsynth * sizeof(XirAotExportSlot));
            *out_nslots = nsynth;
        }
    }
}

/* Recursively collect AOT-eligible protos (those with bb_leaders). */
static void collect_aot_protos(XrProto *proto, XrProto **out, int *count, int max) {
    if (!proto || *count >= max)
        return;
    if (proto->bb_leaders) {
        out[*count] = proto;
        (*count)++;
    }
    for (int i = 0; i < proto->protos.count; i++) {
        XrProto *child = *(XrProto **) xr_dynarray_get_raw(&proto->protos, i);
        collect_aot_protos(child, out, count, max);
    }
}

/* ========== Multi-Module Helpers ========== */

/* Derive a C-safe module name from absolute path.  Caller must free. */
static char *derive_module_name(const char *path) {
    XR_DCHECK(path != NULL, "derive_module_name: NULL path");
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t len = strlen(base);
    if (len > 3 && strcmp(base + len - 3, ".xr") == 0)
        len -= 3;
    char *name = (char *) xr_malloc(len + 1);
    if (!name)
        return NULL;
    for (size_t i = 0; i < len; i++)
        name[i] = (base[i] == '-' || base[i] == '.') ? '_' : base[i];
    name[len] = '\0';
    return name;
}

/* Derive the import string to reach target_path from importer_dir.
 * E.g. "/a/b/math.xr" from "/a/b" → "./math".  Caller must free. */
static char *derive_import_string(const char *target_path, const char *importer_dir) {
    XR_DCHECK(target_path != NULL, "derive_import_string: NULL target");
    XR_DCHECK(importer_dir != NULL, "derive_import_string: NULL dir");
    size_t dir_len = strlen(importer_dir);
    if (strncmp(target_path, importer_dir, dir_len) == 0 && target_path[dir_len] == '/') {
        const char *filename = target_path + dir_len + 1;
        size_t flen = strlen(filename);
        if (flen > 3 && strcmp(filename + flen - 3, ".xr") == 0)
            flen -= 3;
        char *result = (char *) xr_malloc(2 + flen + 1);
        if (!result)
            return NULL;
        result[0] = '.';
        result[1] = '/';
        memcpy(result + 2, filename, flen);
        result[2 + flen] = '\0';
        return result;
    }
    const char *base = strrchr(target_path, '/');
    base = base ? base + 1 : target_path;
    size_t blen = strlen(base);
    if (blen > 3 && strcmp(base + blen - 3, ".xr") == 0)
        blen -= 3;
    char *result = (char *) xr_malloc(2 + blen + 1);
    if (!result)
        return NULL;
    result[0] = '.';
    result[1] = '/';
    memcpy(result + 2, base, blen);
    result[2 + blen] = '\0';
    return result;
}

/* Build cross-module export map for AOT import resolution. */
static XirAotImportEntry *build_export_map(XaotModuleInfo *modules, int nmodules,
                                           int entry_index, int *out_count) {
    *out_count = 0;
    int cap = 64;
    XirAotImportEntry *map = (XirAotImportEntry *) xr_malloc(cap * sizeof(XirAotImportEntry));
    if (!map)
        return NULL;

    char *entry_dir = NULL;
    if (entry_index >= 0) {
        const char *ep = modules[entry_index].path;
        const char *last_slash = strrchr(ep, '/');
        if (last_slash) {
            size_t dlen = (size_t) (last_slash - ep);
            entry_dir = (char *) xr_malloc(dlen + 1);
            if (entry_dir) {
                memcpy(entry_dir, ep, dlen);
                entry_dir[dlen] = '\0';
            }
        }
    }

    for (int m = 0; m < nmodules; m++) {
        if (m == entry_index)
            continue;
        XcgenModule *cmod = modules[m].cmod;
        if (!cmod || cmod->nexports == 0)
            continue;

        char *import_str = entry_dir ? derive_import_string(modules[m].path, entry_dir) : NULL;
        if (!import_str)
            continue;

        for (int e = 0; e < cmod->nexports; e++) {
            if (*out_count >= cap) {
                int new_cap = cap * 2;
                XirAotImportEntry *tmp =
                    (XirAotImportEntry *) xr_realloc(map, new_cap * sizeof(XirAotImportEntry));
                if (!tmp) {
                    xr_free(import_str);
                    xr_free(entry_dir);
                    return map;
                }
                map = tmp;
                cap = new_cap;
            }
            map[*out_count].module_path = import_str;
            map[*out_count].export_name = cmod->exports[e].name;
            map[*out_count].shared_index = cmod->exports[e].shared_index;
            (*out_count)++;
        }
    }
    xr_free(entry_dir);
    return map;
}

/* Assemble the complete C program: headers + transpiled source + main().
 * Returns malloc'd string.  Caller frees via xr_free(). */
static char *assemble_c_source(const char *aot_source, XaotModuleInfo *modules, int nmodules) {
    /* Estimate total size: headers ~100 + source + main (~50 per module) */
    size_t src_len = aot_source ? strlen(aot_source) : 0;
    size_t est = 256 + src_len + (size_t) nmodules * 60;
    char *buf = (char *) xr_malloc(est);
    if (!buf)
        return NULL;
    size_t pos = 0;

    /* Standard headers */
    pos += (size_t) snprintf(buf + pos, est - pos,
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <stdint.h>\n#include <stddef.h>\n\n");

    /* Transpiled functions */
    if (aot_source && src_len > 0) {
        /* Grow buffer if source is larger than estimate */
        if (pos + src_len + 128 > est) {
            est = pos + src_len + 256 + (size_t) nmodules * 60;
            char *tmp = (char *) xr_realloc(buf, est);
            if (!tmp) {
                xr_free(buf);
                return NULL;
            }
            buf = tmp;
        }
        pos += (size_t) snprintf(buf + pos, est - pos,
            "/* --- AOT Transpiled Functions --- */\n%s\n\n", aot_source);
    }

    /* main() calling module inits in topo order */
    pos += (size_t) snprintf(buf + pos, est - pos,
        "/* --- Standalone Main (multi-module, no VM) --- */\n"
        "\nint main(int argc, char **argv) {\n"
        "    (void)argc; (void)argv;\n");

    for (int m = 0; m < nmodules; m++) {
        if (!modules[m].cmod)
            continue;
        const char *init_name = NULL;
        for (int i = 0; i < modules[m].aot_count; i++) {
            XrProto *p = modules[m].aot_protos[i];
            if (!p->name) {
                init_name = modules[m].aot_c_names[i];
                break;
            }
        }
        if (init_name)
            pos += (size_t) snprintf(buf + pos, est - pos, "    %s(NULL);\n", init_name);
    }
    pos += (size_t) snprintf(buf + pos, est - pos,
        "    return 0;\n"
        "}\n");

    return buf;
}

/* ========== Module info cleanup ========== */

static void free_modules(XaotModuleInfo *modules, int nmodules) {
    if (!modules)
        return;
    for (int m = 0; m < nmodules; m++) {
        xr_free((void *) modules[m].path);
        xr_free((void *) modules[m].mod_name);
        xr_free(modules[m].aot_protos);
        xr_free(modules[m].aot_c_names);
        xr_free(modules[m].export_slots);
    }
    xr_free(modules);
}

static void free_import_map(XirAotImportEntry *map, int count) {
    if (!map)
        return;
    const char *prev = NULL;
    for (int i = 0; i < count; i++) {
        if (map[i].module_path != prev) {
            xr_free((void *) map[i].module_path);
            prev = map[i].module_path;
        }
    }
    xr_free(map);
}

/* ========== Public API ========== */

XR_FUNC int xaot_build(const char *input_path, XaotBuildResult *result) {
    XR_DCHECK(input_path != NULL, "xaot_build: NULL input_path");
    XR_DCHECK(result != NULL, "xaot_build: NULL result");
    memset(result, 0, sizeof(*result));

    printf("[native] Building: %s (AOT)\n", input_path);

    /* --- Discover modules via bundle (topo order, entry last) --- */
    int nmodules = 0;
    int entry_index = -1;
    XaotModuleInfo *modules = NULL;
    {
        XrayIsolate *Xb = create_isolate();
        if (!Xb) {
            fprintf(stderr, "Error: failed to create isolate\n");
            return 1;
        }
        XrBundle *bundle = xr_bundle_create_ex(Xb, input_path, XR_BUNDLE_DEFAULT);
        if (!bundle) {
            fprintf(stderr, "Error: bundling failed\n");
            xray_isolate_delete(Xb);
            return 1;
        }
        nmodules = bundle->count;
        modules = (XaotModuleInfo *) xr_calloc(nmodules, sizeof(XaotModuleInfo));
        if (!modules) {
            xr_bundle_free(bundle);
            xray_isolate_delete(Xb);
            return 1;
        }
        for (int i = 0; i < nmodules; i++) {
            modules[i].path = xr_strdup(bundle->entries[i].path);
            modules[i].mod_name = derive_module_name(bundle->entries[i].path);
            if (bundle->entry_path && strcmp(bundle->entries[i].path, bundle->entry_path) == 0)
                entry_index = i;
        }
        xr_bundle_free(bundle);
        xray_isolate_delete(Xb);
    }
    XR_DCHECK(entry_index >= 0, "xaot_build: entry not found in bundle");

    if (nmodules > 1) {
        printf("[native] %d modules (topo order):\n", nmodules);
        for (int i = 0; i < nmodules; i++)
            printf("  [%d] %s%s\n", i, modules[i].path, i == entry_index ? " (entry)" : "");
    }

    /* --- Compile all modules to XrProto --- */
    XrayIsolate *X = create_isolate();
    if (!X) {
        fprintf(stderr, "Error: failed to create isolate\n");
        free_modules(modules, nmodules);
        return 1;
    }

    for (int m = 0; m < nmodules; m++) {
        char *src = read_source_file(modules[m].path);
        if (!src) {
            fprintf(stderr, "Error: cannot read '%s'\n", modules[m].path);
            goto fail_isolate;
        }
        modules[m].proto = xr_compile_source_with_path(X, src, modules[m].path);
        xr_free(src);
        if (!modules[m].proto) {
            fprintf(stderr, "Error: compilation failed for '%s'\n", modules[m].path);
            goto fail_isolate;
        }
    }

    /* --- Create compilation context, collect protos + exports --- */
    XcgenStructRegistry struct_reg;
    xcgen_struct_registry_init(&struct_reg);
    XcgenCompilation *comp = xcgen_compilation_new();
    comp->struct_reg = &struct_reg;

    int shared_cap = 256;
    XrProto **shared_protos = (XrProto **) xr_calloc((size_t) shared_cap, sizeof(XrProto *));
    bool *shared_is_ctor = (bool *) xr_calloc((size_t) shared_cap, sizeof(bool));
    if (!shared_protos || !shared_is_ctor)
        goto fail_comp;
    int nshared = 0;

    int total_aot = 0, total_compiled = 0;

    for (int m = 0; m < nmodules; m++) {
        XrProto *proto = modules[m].proto;
        const char *mname = (m == entry_index) ? "main" : modules[m].mod_name;

        modules[m].cmod = xcgen_compilation_add_module(comp, mname, modules[m].path);
        XR_DCHECK(modules[m].cmod != NULL, "xaot_build: add_module failed");

        collect_exports(proto, modules[m].cmod, &modules[m].export_slots,
                        &modules[m].export_slot_count);

        modules[m].aot_cap = 64;
        modules[m].aot_protos = (XrProto **) xr_malloc(modules[m].aot_cap * sizeof(XrProto *));
        if (!modules[m].aot_protos)
            goto fail_comp;
        modules[m].aot_count = 0;
        collect_aot_protos(proto, modules[m].aot_protos, &modules[m].aot_count, modules[m].aot_cap);

        xcgen_collect_shapes(proto, &struct_reg, (void *) X);
        for (int i = 0; i < modules[m].aot_count; i++)
            xcgen_collect_shapes(modules[m].aot_protos[i], &struct_reg, (void *) X);

        /* Grow shared arrays if needed */
        int needed = proto->shared_offset + (int) proto->code.count;
        if (needed > shared_cap) {
            int new_cap = needed * 2;
            XrProto **tmp_p =
                (XrProto **) xr_realloc(shared_protos, (size_t) new_cap * sizeof(XrProto *));
            bool *tmp_c = (bool *) xr_realloc(shared_is_ctor, (size_t) new_cap * sizeof(bool));
            if (!tmp_p || !tmp_c)
                goto fail_comp;
            memset(tmp_p + shared_cap, 0, (size_t) (new_cap - shared_cap) * sizeof(XrProto *));
            memset(tmp_c + shared_cap, 0, (size_t) (new_cap - shared_cap) * sizeof(bool));
            shared_protos = tmp_p;
            shared_is_ctor = tmp_c;
            shared_cap = new_cap;
        }
        int ns = build_shared_proto_map(proto, shared_protos, shared_is_ctor, shared_cap);
        if (ns > nshared)
            nshared = ns;

        preregister_classes(proto, X, comp);

        modules[m].aot_c_names = (char(*)[140]) xr_malloc(modules[m].aot_count * 140);
        if (!modules[m].aot_c_names && modules[m].aot_count > 0)
            goto fail_comp;

        for (int i = 0; i < modules[m].aot_count; i++) {
            XrProto *p = modules[m].aot_protos[i];
            const char *fname = p->name ? XR_STRING_CHARS(p->name) : "__module_init";
            if (m == entry_index)
                snprintf(modules[m].aot_c_names[i], 140, "xr_%s", fname);
            else
                snprintf(modules[m].aot_c_names[i], 140, "xr_%s__%s", modules[m].mod_name, fname);
            /* Dedup: append index if collision with any prior name */
            for (int pm = 0; pm <= m; pm++) {
                int jmax = (pm == m) ? i : modules[pm].aot_count;
                for (int j = 0; j < jmax; j++) {
                    if (strcmp(modules[m].aot_c_names[i], modules[pm].aot_c_names[j]) == 0) {
                        snprintf(modules[m].aot_c_names[i], 140, "xr_%s__%s_%d",
                                 modules[m].mod_name, fname, total_aot + i);
                        goto dedup_done;
                    }
                }
            }
        dedup_done:
            xcgen_register_proto(comp, (void *) modules[m].aot_protos[i],
                                 modules[m].aot_c_names[i]);
        }
        total_aot += modules[m].aot_count;
    }

    xcgen_rebuild_field_index(&struct_reg);

    /* --- Build cross-module export map --- */
    int import_map_count = 0;
    XirAotImportEntry *import_map =
        build_export_map(modules, nmodules, entry_index, &import_map_count);
    if (import_map_count > 0)
        printf("[native] Cross-module exports: %d entries\n", import_map_count);

    /* --- Lower each function: XIR → optimize → C --- */
    for (int m = 0; m < nmodules; m++) {
        if (nmodules > 1)
            printf("[native] Module: %s (%d functions)\n", modules[m].mod_name,
                   modules[m].aot_count);

        for (int i = 0; i < modules[m].aot_count; i++) {
            XrProto *p = modules[m].aot_protos[i];
            const char *fname = p->name ? XR_STRING_CHARS(p->name) : "__module_init";

            XirFunc *xfunc;
            bool is_init = (p->name == NULL);
            bool needs_opts = is_init && (import_map_count > 0 || modules[m].export_slot_count > 0);
            if (needs_opts) {
                XirAotOptions opts = {
                    .import_map = import_map,
                    .import_count = import_map_count,
                    .export_slots = modules[m].export_slots,
                    .export_slot_count = modules[m].export_slot_count,
                };
                xfunc = xir_build_from_proto_aot_ex(p, shared_protos, nshared, X, &opts);
            } else {
                xfunc = xir_build_from_proto_aot(p, shared_protos, nshared, X);
            }

            if (!xfunc) {
                printf("  [C] %s → skip (XIR build failed)\n", fname);
                continue;
            }
            xir_run_pipeline(xfunc, XIR_OPT_FULL);
            XcgenFunc *cf = xcgen_compile_func(modules[m].cmod, xfunc, modules[m].aot_c_names[i]);
            xir_func_destroy(xfunc);
            if (cf) {
                printf("  [C] %s → %zu bytes\n", fname, cf->body.len);
                total_compiled++;
            } else {
                printf("  [C] %s → skip (xcgen failed)\n", fname);
            }
        }
    }

    xray_isolate_delete(X);
    X = NULL;

    printf("AOT: %d/%d functions transpiled across %d modules\n", total_compiled, total_aot,
           nmodules);

    /* --- Emit C source + main() --- */
    char *aot_source = (total_compiled > 0) ? xcgen_emit_source(comp) : NULL;
    xr_free(shared_protos);
    xr_free(shared_is_ctor);
    xcgen_compilation_free(comp);
    comp = NULL;
    free_import_map(import_map, import_map_count);

    /* Assemble complete C program in memory */
    char *c_source = assemble_c_source(aot_source, modules, nmodules);
    xr_free(aot_source);
    if (!c_source) {
        fprintf(stderr, "Error: failed to assemble C source\n");
        free_modules(modules, nmodules);
        return 1;
    }

    result->c_source = c_source;
    result->total_compiled = total_compiled;
    result->total_aot = total_aot;
    result->nmodules = nmodules;

    free_modules(modules, nmodules);
    return 0;

    /* Error cleanup paths */
fail_comp:
    xr_free(shared_protos);
    xr_free(shared_is_ctor);
    xcgen_compilation_free(comp);
fail_isolate:
    xray_isolate_delete(X);
    free_modules(modules, nmodules);
    return 1;
}
