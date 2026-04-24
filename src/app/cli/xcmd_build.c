/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xcmd_build.c - 'xray build' command implementation
 *
 * KEY CONCEPT:
 *   Two build modes:
 *   1. Default: bytecode embedding (full xray features, needs runtime)
 *   2. --native: AOT + bytecode hybrid, links xray runtime (like Go)
 *
 * WHY THIS DESIGN:
 *   - Bytecode mode supports all xray features (coroutines, dynamic types)
 *   - Native mode combines AOT performance with full runtime support
 */

#include "xcli.h"
#include "xcli_spec.h"
#include "xcli_fs.h"
#include "xcli_isolate.h"
#include "xray.h"
#include "xray_isolate.h"
#include "../../runtime/xisolate_api.h"
#include "../../module/xbytecode_io.h"
#include "../../module/xbundle.h"
#include "../../runtime/value/xchunk.h"
#include "../../runtime/value/xslot_type.h"
#include "../../runtime/value/xtype.h"
#include "../../runtime/object/xstring.h"
#include "../../base/xdynarray.h"
#ifdef XRAY_HAS_JIT
#include "../../jit/xir_builder.h"
#include "../../jit/xir_pass.h"
#include "../../aot/xcgen.h"
#include "../../runtime/class/xclass_descriptor.h"
#include "../../runtime/class/xclass.h"
#include "../../runtime/class/xclass_lookup.h"
#include "../../runtime/closure/xclosure.h"
#endif
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>

/* ========== Shared Helpers ========== */

// Invoke C compiler to link generated C source (+ optional .o) into executable
static int invoke_cc(const char *cc, const char *opt_flag, const char *output_file,
                     const char *c_file, const char *obj_file,
                     bool strip_symbols, const char *sysroot) {
    const char *xray_include = getenv("XRAY_INCLUDE");
    const char *xray_lib = getenv("XRAY_LIB");

    char include_path[512], lib_path[512];
    if (sysroot) {
        snprintf(include_path, sizeof(include_path), "%s/include/xray", sysroot);
        snprintf(lib_path, sizeof(lib_path), "%s/lib", sysroot);
        xray_include = include_path;
        xray_lib = lib_path;
    } else {
        if (!xray_include) xray_include = "/usr/local/include/xray";
        if (!xray_lib) xray_lib = "/usr/local/lib";
    }

    char include_flag[600], lib_flag[600];
    snprintf(include_flag, sizeof(include_flag), "-I%s", xray_include);
    snprintf(lib_flag, sizeof(lib_flag), "-L%s", xray_lib);

    const char *spawn_argv[26];
    int ai = 0;
    spawn_argv[ai++] = cc;
    spawn_argv[ai++] = opt_flag;
    spawn_argv[ai++] = "-o";
    spawn_argv[ai++] = output_file;
    spawn_argv[ai++] = c_file;
    if (obj_file) spawn_argv[ai++] = obj_file;
    spawn_argv[ai++] = include_flag;
#ifdef XRT_AOT_INCLUDE_DIR
    spawn_argv[ai++] = "-I" XRT_AOT_INCLUDE_DIR;
#endif
    spawn_argv[ai++] = lib_flag;
    spawn_argv[ai++] = "-lxray_core";
    spawn_argv[ai++] = "-lpthread";
    spawn_argv[ai++] = "-lm";
#ifdef __APPLE__
    spawn_argv[ai++] = "-Wl,-dead_strip";
#else
    spawn_argv[ai++] = "-Wl,--gc-sections";
#endif
    if (strip_symbols) spawn_argv[ai++] = "-Wl,-x";
    spawn_argv[ai] = NULL;

    printf("Linking:");
    for (int i = 0; spawn_argv[i]; i++) printf(" %s", spawn_argv[i]);
    printf("\n");

    extern char **environ;
    pid_t pid;
    int spawn_err = posix_spawnp(&pid, cc, NULL, NULL, (char *const *)spawn_argv, environ);
    if (spawn_err != 0) {
        fprintf(stderr, "Error: failed to start compiler '%s': %s\n", cc, strerror(spawn_err));
        return 1;
    }

    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Error: linking failed\n");
        fprintf(stderr, "Tip: set XRAY_INCLUDE and XRAY_LIB environment variables\n");
        fprintf(stderr, "  export XRAY_INCLUDE=/path/to/xray/include\n");
        fprintf(stderr, "  export XRAY_LIB=/path/to/xray/build\n");
        return 1;
    }
    return 0;
}

// Invoke C compiler for standalone AOT binary (no libxray_core)
static int invoke_cc_standalone(const char *cc, const char *opt_flag,
                                const char *output_file, const char *c_file,
                                bool strip_symbols, const char *sysroot) {
    char aot_include[600] = "";
#ifdef XRT_AOT_INCLUDE_DIR
    snprintf(aot_include, sizeof(aot_include), "-I" XRT_AOT_INCLUDE_DIR);
#else
    const char *xray_include = getenv("XRAY_INCLUDE");
    if (sysroot) {
        snprintf(aot_include, sizeof(aot_include), "-I%s/include/xray", sysroot);
    } else if (xray_include) {
        snprintf(aot_include, sizeof(aot_include), "-I%s", xray_include);
    } else {
        snprintf(aot_include, sizeof(aot_include), "-I/usr/local/include/xray");
    }
#endif

    const char *spawn_argv[16];
    int ai = 0;
    spawn_argv[ai++] = cc;
    spawn_argv[ai++] = opt_flag;
    spawn_argv[ai++] = "-o";
    spawn_argv[ai++] = output_file;
    spawn_argv[ai++] = c_file;
    if (aot_include[0]) spawn_argv[ai++] = aot_include;
    spawn_argv[ai++] = "-lm";
#ifdef __APPLE__
    spawn_argv[ai++] = "-Wl,-dead_strip";
#else
    spawn_argv[ai++] = "-Wl,--gc-sections";
#endif
    if (strip_symbols) spawn_argv[ai++] = "-Wl,-x";
    spawn_argv[ai] = NULL;

    printf("Linking (standalone):");
    for (int i = 0; spawn_argv[i]; i++) printf(" %s", spawn_argv[i]);
    printf("\n");

    extern char **environ;
    pid_t pid;
    int spawn_err = posix_spawnp(&pid, cc, NULL, NULL,
                                  (char *const *)spawn_argv, environ);
    if (spawn_err != 0) {
        fprintf(stderr, "Error: failed to start compiler '%s': %s\n",
                cc, strerror(spawn_err));
        return 1;
    }

    int status;
    waitpid(pid, &status, 0);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Error: standalone linking failed\n");
        return 1;
    }
    return 0;
}

// Write the main() function for bytecode execution into a C file
// bundle_source is the output of xr_bundle_to_c_source()
static void write_bytecode_main(FILE *f, const char *bundle_source) {
    // Headers (bundle source uses strcmp but doesn't include string.h)
    fprintf(f,
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <stdint.h>\n"
        "#include <stddef.h>\n\n"
    );

    // Bundle-generated data (module bytecode arrays, module table, lookup function)
    fprintf(f, "%s\n\n", bundle_source);

    // Main: use XR_INIT_RUNTIME for minimal footprint (no compiler/analyzer/stdlib)
    fprintf(f,
        "#include \"xray_isolate.h\"\n"
        "extern int xr_eval_bytecode(void*, const uint8_t*, size_t);\n"
        "extern void xr_multicore_init(void*, int);\n"
        "extern void xr_multicore_destroy(void*);\n"
        "\n"
        "int main(int argc, char **argv) {\n"
        "    XrayIsolateParams params;\n"
        "    xray_isolate_params_init(&params);\n"
        "    params.init_flags = XR_INIT_RUNTIME;\n"
        "    params.script_argc = argc > 1 ? argc - 1 : 0;\n"
        "    params.script_argv = argc > 1 ? argv + 1 : NULL;\n"
        "    XrayIsolate *X = xray_isolate_new(&params);\n"
        "    if (!X) { fprintf(stderr, \"Failed to create runtime\\n\"); return 1; }\n"
        "    xr_multicore_init(X, 0);\n"
        "    const XrEmbeddedModule *entry = &xr_app_modules[xr_app_entry_index];\n"
        "    int result = xr_eval_bytecode(X, entry->bc, entry->size);\n"
        "    xr_multicore_destroy(X);\n"
        "    xray_isolate_delete(X);\n"
        "    return result;\n"
        "}\n"
    );
}

/* ========== Optimization Flag ========== */

static const char *make_opt_flag(const char *level) {
    if (!level) return "-O2";
    if (strcmp(level, "0") == 0) return "-O0";
    if (strcmp(level, "1") == 0) return "-O1";
    if (strcmp(level, "2") == 0) return "-O2";
    if (strcmp(level, "3") == 0) return "-O3";
    if (strcmp(level, "s") == 0) return "-Os";
    if (strcmp(level, "fast") == 0) return "-Ofast";
    fprintf(stderr, "Warning: unknown optimization level '%s', using -O2\n", level);
    return "-O2";
}

/* ========== Build Sub-Modes (forward declarations) ========== */

static int cmd_build_bytecode(const char *input, const char *output,
                              const char *cc, const char *opt_flag,
                              bool c_only, bool strip, const char *sysroot);
static int cmd_build_native(const char *input, const char *output,
                            const char *cc, const char *opt_flag,
                            bool c_only, bool strip, const char *sysroot);

/* ========== CLI Entry Point ========== */

XR_FUNC int cmd_build(const XrCliInvocation *inv) {
    XR_DCHECK(inv != NULL, "inv is NULL");
    XR_DCHECK(inv->positional_count == 1, "build expects exactly 1 positional");

    const char *input_file  = inv->positionals[0];
    const char *output_file = xr_cli_opt_string(&inv->options, "output", NULL);
    const char *cc          = xr_cli_opt_string(&inv->options, "cc", "cc");
    const char *opt_level   = xr_cli_opt_string(&inv->options, "opt", NULL);
    const char *sysroot     = xr_cli_opt_string(&inv->options, "sysroot", NULL);
    bool c_only             = xr_cli_opt_bool(&inv->options, "c-only");
    bool strip_symbols      = xr_cli_opt_bool(&inv->options, "strip");
    bool native_mode        = xr_cli_opt_bool(&inv->options, "native");

    if (!output_file) output_file = c_only ? "app.c" : "a.out";

    const char *opt_flag = make_opt_flag(opt_level);

    if (native_mode) {
        return cmd_build_native(input_file, output_file, cc, opt_flag, c_only, strip_symbols, sysroot);
    }
    return cmd_build_bytecode(input_file, output_file, cc, opt_flag, c_only, strip_symbols, sysroot);
}

/* ========== Bytecode Bundling (default mode) ========== */

static int cmd_build_bytecode(const char *input, const char *output,
                              const char *cc, const char *opt_flag,
                              bool c_only, bool strip, const char *sysroot) {
    printf("[bytecode] Building: %s\n", input);

    XrayIsolate *X = xr_cli_isolate_new(XR_CLI_ISOLATE_RUN);
    if (!X) { fprintf(stderr, "Error: failed to create isolate\n"); return 1; }

    XrBundle *bundle = xr_bundle_create_ex(X, input, XR_BUNDLE_DEFAULT);
    if (!bundle) {
        fprintf(stderr, "Error: bytecode bundling failed\n");
        xray_isolate_delete(X);
        return 1;
    }
    xray_isolate_delete(X);

    printf("Modules: %d\n", bundle->count);
    for (int i = 0; i < bundle->count; i++)
        printf("  %s (%zu bytes)\n", bundle->entries[i].path, bundle->entries[i].bc_size);

    char *bc_source = xr_bundle_to_c_source(bundle, "xr_app");
    xr_bundle_free(bundle);
    if (!bc_source) { fprintf(stderr, "Error: C source generation failed\n"); return 1; }

    char c_file[512];
    if (c_only) snprintf(c_file, sizeof(c_file), "%s", output);
    else snprintf(c_file, sizeof(c_file), "/tmp/xray_bc_%d.c", getpid());

    FILE *f = fopen(c_file, "w");
    if (!f) { fprintf(stderr, "Error: cannot create '%s'\n", c_file); xr_free(bc_source); return 1; }
    write_bytecode_main(f, bc_source);
    fclose(f);
    xr_free(bc_source);

    if (c_only) { printf("Generated: %s\n", output); return 0; }

    int ret = invoke_cc(cc, opt_flag, output, c_file, NULL, strip, sysroot);
    unlink(c_file);
    if (ret == 0) printf("Generated: %s\n", output);
    return ret;
}

/* ========== Native Build Helpers ========== */

#ifdef XRAY_HAS_JIT

// Build shared_index → child_proto mapping by scanning top-level bytecode.
// Recognizes two patterns:
//   1) CLOSURE R[A], Proto[Bx]; SETSHARED R[A], G[j]  — plain closure
//   2) CLOSUREs R[X]...; CLASS_FROM_DESC R[X]; [MOVE;] SETSHARED R[X], G[j]
//      → maps to constructor proto (first CLOSURE before CLASS_FROM_DESC)
// shared_is_ctor[i] is set when shared_protos[i] is a class constructor.
// Returns max shared index + 1 (size of shared_protos array).
static int build_shared_proto_map(XrProto *top, XrProto **shared_protos,
                                   bool *shared_is_ctor, int max_shared) {
    if (!top) return 0;
    int max_idx = 0;
    uint32_t code_count = (uint32_t)top->code.count;
    const XrInstruction *code = (const XrInstruction *)top->code.data;
    for (uint32_t pc = 0; pc + 1 < code_count; pc++) {
        XrInstruction inst0 = code[pc];
        XrInstruction inst1 = code[pc + 1];
        OpCode op0 = GET_OPCODE(inst0);
        OpCode op1 = GET_OPCODE(inst1);

        // Pattern 1: CLOSURE R[A]; SETSHARED R[A]
        if (op0 == OP_CLOSURE && op1 == OP_SETSHARED) {
            int closure_dst = GETARG_A(inst0);
            int setshared_src = GETARG_A(inst1);
            if (closure_dst == setshared_src) {
                uint16_t proto_idx = GETARG_Bx(inst0);
                int shared_bx = GETARG_Bx(inst1);
                int abs_idx = shared_bx + top->shared_offset;
                if (abs_idx >= 0 && abs_idx < max_shared &&
                    proto_idx < PROTO_PROTO_COUNT(top)) {
                    shared_protos[abs_idx] = PROTO_PROTO(top, proto_idx);
                    if (abs_idx + 1 > max_idx) max_idx = abs_idx + 1;
                }
            }
        }

        // Pattern 2: CLASS_FROM_DESC R[X]; [MOVE R[Y] R[X];] SETSHARED R[X]
        if (op0 == OP_CLASS_CREATE_FROM_DESCRIPTOR) {
            int class_reg = GETARG_A(inst0);
            // Look ahead for SETSHARED (with optional MOVE in between)
            int ss_pc = -1;
            if (op1 == OP_SETSHARED && GETARG_A(inst1) == class_reg) {
                ss_pc = (int)pc + 1;
            } else if (op1 == OP_MOVE && pc + 2 < code_count) {
                XrInstruction inst2 = code[pc + 2];
                if (GET_OPCODE(inst2) == OP_SETSHARED &&
                    GETARG_A(inst2) == class_reg) {
                    ss_pc = (int)pc + 2;
                }
            }
            if (ss_pc >= 0) {
                int shared_bx = GETARG_Bx(code[ss_pc]);
                int abs_idx = shared_bx + top->shared_offset;
                // Scan backward for first CLOSURE targeting class_reg
                int ctor_pc = -1;
                for (int scan = (int)pc - 1; scan >= 0; scan--) {
                    OpCode sop = GET_OPCODE(code[scan]);
                    if (sop == OP_CLOSURE && GETARG_A(code[scan]) == class_reg)
                        ctor_pc = scan; // keep updating → earliest wins
                    else if (ctor_pc >= 0)
                        break; // passed CLOSURE block
                }
                if (ctor_pc >= 0 && abs_idx >= 0 && abs_idx < max_shared) {
                    uint16_t ctor_proto_idx = GETARG_Bx(code[ctor_pc]);
                    if (ctor_proto_idx < PROTO_PROTO_COUNT(top)) {
                        shared_protos[abs_idx] = PROTO_PROTO(top, ctor_proto_idx);
                        shared_is_ctor[abs_idx] = true;
                        if (abs_idx + 1 > max_idx) max_idx = abs_idx + 1;
                    }
                }
            }
        }
    }
    return max_idx;
}

// Pre-register classes in isolate for CHA devirtualization.
// Without VM execution, classes are never instantiated. This scans
// top-level bytecode for CLASS_FROM_DESC and creates minimal class
// objects so xr_class_lookup_by_name works at AOT compile time.
static void aot_preregister_classes(XrProto *proto, XrayIsolate *isolate) {
    if (!proto || !isolate) return;
    uint32_t code_count = (uint32_t)proto->code.count;
    const XrInstruction *code = (const XrInstruction *)proto->code.data;
    for (uint32_t pc = 0; pc < code_count; pc++) {
        XrInstruction inst = code[pc];
        if (GET_OPCODE(inst) != OP_CLASS_CREATE_FROM_DESCRIPTOR) continue;
        int bx = GETARG_Bx(inst);
        if (bx >= (int)PROTO_CONST_COUNT(proto)) continue;
        XrValue desc_val = PROTO_CONSTANT(proto, bx);
        XrClassDescriptor *desc = (XrClassDescriptor *)XR_TO_PTR(desc_val);
        if (!desc) continue;
        XrClass *klass = xr_class_from_descriptor(isolate, desc, proto, NULL, NULL, NULL, NULL);
        if (!klass) continue;

        // Patch method protos: set 'this' (param 0) type to the
        // enclosing class instance type so builder_find_reg_type works.
        for (uint32_t mi = 0; mi < desc->instance_method_count && mi < klass->method_count; mi++) {
            XrMethod *method = &klass->methods[mi];
            if (method->type != XMETHOD_CLOSURE || !method->as.closure) continue;
            XrProto *mp = method->as.closure->proto;
            if (!mp || mp->numparams < 1 || mp->numparams > 200) continue;
            // Allocate param_types if not present
            if (!mp->param_types) {
                mp->param_types = (struct XrType **)xr_calloc(
                    (size_t)mp->numparams, sizeof(struct XrType *));
                if (!mp->param_types) continue;
                mp->param_types_count = (uint8_t)mp->numparams;
            }
            // Patch 'this' (param 0) with enclosing class instance type
            if (mp->param_types_count > 0 && !mp->param_types[0]) {
                mp->param_types[0] = xr_type_new_named_instance(isolate, desc->class_name);
            }
        }
    }
}

// Scan bytecodes for OP_EXPORT and correlate with OP_SETSHARED to populate exports.
// Pattern: OP_SETSHARED R[A], shared_idx; ... OP_EXPORT K[name_idx], R[A], is_const
// For const exports without SETSHARED, allocates synthetic shared indices and
// populates out_slots/out_nslots so the XIR builder can emit SETSHARED at export time.
static void collect_exports(XrProto *proto, XcgenModule *mod,
                            XirAotExportSlot **out_slots, int *out_nslots) {
    if (out_slots) *out_slots = NULL;
    if (out_nslots) *out_nslots = 0;
    if (!proto || !mod) return;
    uint32_t code_count = (uint32_t)proto->code.count;
    const XrInstruction *code = (const XrInstruction *)proto->code.data;

    // Find max local Bx in SETSHARED to allocate synthetic indices beyond it
    int max_local_bx = -1;
    for (uint32_t pc = 0; pc < code_count; pc++) {
        if (GET_OPCODE(code[pc]) == OP_SETSHARED) {
            int bx = GETARG_Bx(code[pc]);
            if (bx > max_local_bx) max_local_bx = bx;
        }
    }
    int next_local_bx = max_local_bx + 1;

    // Temporary storage for synthetic export slots
    XirAotExportSlot synth[64];
    int nsynth = 0;

    for (uint32_t pc = 0; pc < code_count; pc++) {
        XrInstruction inst = code[pc];
        if (GET_OPCODE(inst) != OP_EXPORT) continue;

        int name_idx = GETARG_A(inst);
        int value_reg = GETARG_B(inst);
        int is_const = GETARG_C(inst);

        // Get export name from constant pool
        if (name_idx >= (int)VALUEARRAY_COUNT(&proto->constants)) continue;
        XrValue name_val = VALUEARRAY_GET(&proto->constants, name_idx);
        if (!XR_IS_STRING(name_val)) continue;
        const char *export_name = XR_STRING_CHARS(XR_TO_STRING(name_val));

        // Scan backwards for the shared_index, tracing through OP_MOVE chains.
        // Two-pass: (1) collect all registers in the MOVE chain,
        // (2) scan for GETSHARED/SETSHARED matching any of them.
        int shared_idx = -1;
        int chain[16];
        int chain_len = 0;
        chain[chain_len++] = value_reg;
        for (int bpc = (int)pc - 1; bpc >= 0 && chain_len < 16; bpc--) {
            XrInstruction prev = code[bpc];
            if (GET_OPCODE(prev) == OP_MOVE &&
                GETARG_A(prev) == chain[chain_len - 1]) {
                chain[chain_len++] = GETARG_B(prev);
            }
        }
        for (int bpc = (int)pc - 1; bpc >= 0 && shared_idx < 0; bpc--) {
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
            // Allocate a synthetic shared index for this const export
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

    // Copy synthetic slots to caller-owned array
    if (nsynth > 0 && out_slots && out_nslots) {
        *out_slots = (XirAotExportSlot *)xr_malloc(
                         nsynth * sizeof(XirAotExportSlot));
        if (*out_slots) {
            memcpy(*out_slots, synth, nsynth * sizeof(XirAotExportSlot));
            *out_nslots = nsynth;
        }
    }
}

// Recursively collect AOT-eligible protos (bb_leaders required)
// Untyped parameters are treated as tagged (XR_SLOT_ANY) — lower performance
// but still faster than VM interpreter for complex function bodies.
static void collect_aot_protos(XrProto *proto, XrProto **out, int *count, int max) {
    if (!proto || *count >= max) return;

    if (proto->bb_leaders) {
        out[*count] = proto;
        (*count)++;
    }

    for (int i = 0; i < proto->protos.count; i++) {
        XrProto *child = *(XrProto **)xr_dynarray_get_raw(&proto->protos, i);
        collect_aot_protos(child, out, count, max);
    }
}

/* ========== Multi-Module AOT Helpers ========== */

// Per-module state during AOT compilation
typedef struct {
    const char   *path;              // absolute source path (owned)
    const char   *mod_name;          // module name for C codegen (owned)
    XrProto      *proto;             // compiled bytecode (owned by isolate)
    XcgenModule  *cmod;              // C codegen module (owned by comp)
    XrProto     **aot_protos;        // AOT-eligible protos (owned)
    char        (*aot_c_names)[140]; // C function names (owned)
    int           aot_count;
    int           aot_cap;
    // Synthetic export slots for const exports without OP_SETSHARED
    XirAotExportSlot *export_slots;  // owned, passed to XIR builder
    int               export_slot_count;
} AotModuleInfo;

// Derive a C-safe module name from absolute path (caller must free)
static char *aot_derive_module_name(const char *path) {
    XR_DCHECK(path != NULL, "aot_derive_module_name: NULL path");
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    size_t len = strlen(base);
    // Strip .xr extension
    if (len > 3 && strcmp(base + len - 3, ".xr") == 0)
        len -= 3;
    char *name = (char *)xr_malloc(len + 1);
    if (!name) return NULL;
    for (size_t i = 0; i < len; i++)
        name[i] = (base[i] == '-' || base[i] == '.') ? '_' : base[i];
    name[len] = '\0';
    return name;
}

// Derive the import string that would be used to reach target_path from
// importer_dir. E.g. "/a/b/math.xr" from "/a/b" → "./math"
static char *aot_derive_import_string(const char *target_path,
                                       const char *importer_dir) {
    XR_DCHECK(target_path != NULL, "aot_derive_import_string: NULL target");
    XR_DCHECK(importer_dir != NULL, "aot_derive_import_string: NULL dir");
    size_t dir_len = strlen(importer_dir);
    // Check if target is in the same directory
    if (strncmp(target_path, importer_dir, dir_len) == 0 &&
        target_path[dir_len] == '/') {
        const char *filename = target_path + dir_len + 1;
        size_t flen = strlen(filename);
        // Strip .xr extension
        if (flen > 3 && strcmp(filename + flen - 3, ".xr") == 0)
            flen -= 3;
        // Build "./basename"
        char *result = (char *)xr_malloc(2 + flen + 1);
        if (!result) return NULL;
        result[0] = '.';
        result[1] = '/';
        memcpy(result + 2, filename, flen);
        result[2 + flen] = '\0';
        return result;
    }
    // Fallback: use basename
    const char *base = strrchr(target_path, '/');
    base = base ? base + 1 : target_path;
    size_t blen = strlen(base);
    if (blen > 3 && strcmp(base + blen - 3, ".xr") == 0) blen -= 3;
    char *result = (char *)xr_malloc(2 + blen + 1);
    if (!result) return NULL;
    result[0] = '.'; result[1] = '/';
    memcpy(result + 2, base, blen);
    result[2 + blen] = '\0';
    return result;
}

// Build cross-module export map for AOT import resolution.
// For each non-entry module, creates entries mapping
// (import_string, export_name) → absolute shared index.
static XirAotImportEntry *aot_build_export_map(AotModuleInfo *modules,
                                                 int nmodules,
                                                 int entry_index,
                                                 int *out_count) {
    *out_count = 0;
    int cap = 64;
    XirAotImportEntry *map = (XirAotImportEntry *)xr_malloc(
        cap * sizeof(XirAotImportEntry));
    if (!map) return NULL;

    // Derive the entry module's directory for relative path resolution
    char *entry_dir = NULL;
    if (entry_index >= 0) {
        const char *ep = modules[entry_index].path;
        const char *last_slash = strrchr(ep, '/');
        if (last_slash) {
            size_t dlen = (size_t)(last_slash - ep);
            entry_dir = (char *)xr_malloc(dlen + 1);
            if (entry_dir) { memcpy(entry_dir, ep, dlen); entry_dir[dlen] = '\0'; }
        }
    }

    for (int m = 0; m < nmodules; m++) {
        if (m == entry_index) continue; // Entry module is not imported
        XcgenModule *cmod = modules[m].cmod;
        if (!cmod || cmod->nexports == 0) continue;

        // Compute what import string would reference this module
        char *import_str = entry_dir
            ? aot_derive_import_string(modules[m].path, entry_dir)
            : NULL;
        if (!import_str) continue;

        for (int e = 0; e < cmod->nexports; e++) {
            // Grow if needed
            if (*out_count >= cap) {
                int new_cap = cap * 2;
                XirAotImportEntry *tmp = (XirAotImportEntry *)xr_realloc(
                    map, new_cap * sizeof(XirAotImportEntry));
                if (!tmp) { xr_free(import_str); xr_free(entry_dir); return map; }
                map = tmp;
                cap = new_cap;
            }
            map[*out_count].module_path = import_str; // shared, freed later
            map[*out_count].export_name = cmod->exports[e].name;
            map[*out_count].shared_index = cmod->exports[e].shared_index;
            (*out_count)++;
        }
        // import_str is shared by all entries for this module — do NOT free here.
        // Lifetime: valid until cmd_build_native returns.
    }
    xr_free(entry_dir);
    return map;
}

// Write multi-module standalone main() to file
static void aot_write_main(FILE *f, AotModuleInfo *modules, int nmodules) {
    fprintf(f,
        "/* --- Standalone Main (multi-module, no VM) --- */\n"
        "\nint main(int argc, char **argv) {\n"
        "    (void)argc; (void)argv;\n");
    // Call module inits in topo order (bundle order = leaves first, entry last)
    for (int m = 0; m < nmodules; m++) {
        if (!modules[m].cmod) continue;
        // Find the init function name (proto with no name = top-level)
        const char *init_name = NULL;
        for (int i = 0; i < modules[m].aot_count; i++) {
            XrProto *p = modules[m].aot_protos[i];
            if (!p->name) {
                init_name = modules[m].aot_c_names[i];
                break;
            }
        }
        if (init_name) {
            fprintf(f, "    %s(NULL);\n", init_name);
        }
    }
    fprintf(f,
        "    return 0;\n"
        "}\n");
}

/* ========== Native Build (--native, standalone AOT) ========== */

static int cmd_build_native(const char *input, const char *output,
                            const char *cc, const char *opt_flag,
                            bool c_only, bool strip, const char *sysroot) {
    printf("[native] Building: %s (AOT)\n", input);

    // Phase 1: Discover modules via bundle (topo order, entry last)
    int nmodules = 0;
    int entry_index = -1;
    AotModuleInfo *modules = NULL;
    {
        XrayIsolate *Xb = xr_cli_isolate_new(XR_CLI_ISOLATE_RUN);
        if (!Xb) { fprintf(stderr, "Error: failed to create isolate\n"); return 1; }
        XrBundle *bundle = xr_bundle_create_ex(Xb, input, XR_BUNDLE_DEFAULT);
        if (!bundle) {
            fprintf(stderr, "Error: bundling failed\n");
            xray_isolate_delete(Xb); return 1;
        }
        nmodules = bundle->count;
        modules = (AotModuleInfo *)xr_calloc(nmodules, sizeof(AotModuleInfo));
        if (!modules) {
            xr_bundle_free(bundle); xray_isolate_delete(Xb); return 1;
        }
        for (int i = 0; i < nmodules; i++) {
            modules[i].path = xr_strdup(bundle->entries[i].path);
            modules[i].mod_name = aot_derive_module_name(bundle->entries[i].path);
            if (bundle->entry_path &&
                strcmp(bundle->entries[i].path, bundle->entry_path) == 0)
                entry_index = i;
        }
        xr_bundle_free(bundle);
        xray_isolate_delete(Xb);
    }
    XR_DCHECK(entry_index >= 0, "cmd_build_native: entry not found in bundle");

    if (nmodules > 1) {
        printf("[native] %d modules (topo order):\n", nmodules);
        for (int i = 0; i < nmodules; i++)
            printf("  [%d] %s%s\n", i, modules[i].path,
                   i == entry_index ? " (entry)" : "");
    }

    // Phase 2: Compile all modules to XrProto (topo order, shared offsets accumulate)
    XrayIsolate *X = xr_cli_isolate_new(XR_CLI_ISOLATE_RUN);
    if (!X) { fprintf(stderr, "Error: failed to create isolate\n"); goto fail_early; }

    for (int m = 0; m < nmodules; m++) {
        char *src = xr_cli_read_file(modules[m].path);
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

    // Phase 3: Create compilation context, collect protos + exports
    XcgenStructRegistry struct_reg;
    xcgen_struct_registry_init(&struct_reg);
    XcgenCompilation *comp = xcgen_compilation_new();
    comp->struct_reg = &struct_reg;

    // Global shared_protos array (all modules combined)
    XrProto *shared_protos[256];
    bool shared_is_ctor[256];
    memset(shared_protos, 0, sizeof(shared_protos));
    memset(shared_is_ctor, 0, sizeof(shared_is_ctor));
    int nshared = 0;

    int total_aot = 0, total_compiled = 0;

    for (int m = 0; m < nmodules; m++) {
        XrProto *proto = modules[m].proto;
        const char *mname = (m == entry_index) ? "main" : modules[m].mod_name;

        // Add module to compilation
        modules[m].cmod = xcgen_compilation_add_module(comp, mname, modules[m].path);
        XR_DCHECK(modules[m].cmod != NULL, "cmd_build_native: add_module failed");

        // Collect exports (+ synthetic const export slots)
        collect_exports(proto, modules[m].cmod,
                        &modules[m].export_slots,
                        &modules[m].export_slot_count);

        // Collect AOT-eligible protos
        modules[m].aot_cap = 64;
        modules[m].aot_protos = (XrProto **)xr_malloc(
            modules[m].aot_cap * sizeof(XrProto *));
        if (!modules[m].aot_protos) goto fail_comp;
        modules[m].aot_count = 0;
        collect_aot_protos(proto, modules[m].aot_protos,
                           &modules[m].aot_count, modules[m].aot_cap);

        // Collect shapes for struct promotion
        xcgen_collect_shapes(proto, &struct_reg, (void *)X);
        for (int i = 0; i < modules[m].aot_count; i++)
            xcgen_collect_shapes(modules[m].aot_protos[i], &struct_reg, (void *)X);

        // Build shared_proto_map for this module
        int ns = build_shared_proto_map(proto, shared_protos, shared_is_ctor, 256);
        if (ns > nshared) nshared = ns;

        // Pre-register classes
        aot_preregister_classes(proto, X);

        // Allocate C name storage
        modules[m].aot_c_names = (char (*)[140])xr_malloc(
            modules[m].aot_count * 140);
        if (!modules[m].aot_c_names && modules[m].aot_count > 0) goto fail_comp;

        // Generate unique C function names with module prefix
        for (int i = 0; i < modules[m].aot_count; i++) {
            XrProto *p = modules[m].aot_protos[i];
            const char *fname = p->name ? XR_STRING_CHARS(p->name) : "__module_init";
            if (m == entry_index)
                snprintf(modules[m].aot_c_names[i], 140, "xr_%s", fname);
            else
                snprintf(modules[m].aot_c_names[i], 140, "xr_%s__%s",
                         modules[m].mod_name, fname);
            // Dedup: append index if collision with ANY prior name
            for (int pm = 0; pm <= m; pm++) {
                int jmax = (pm == m) ? i : modules[pm].aot_count;
                for (int j = 0; j < jmax; j++) {
                    if (strcmp(modules[m].aot_c_names[i],
                               modules[pm].aot_c_names[j]) == 0) {
                        snprintf(modules[m].aot_c_names[i], 140,
                                 "xr_%s__%s_%d", modules[m].mod_name, fname,
                                 total_aot + i);
                        goto dedup_done;
                    }
                }
            }
            dedup_done:
            xcgen_register_proto(comp, (void *)modules[m].aot_protos[i],
                                 modules[m].aot_c_names[i]);
        }
        total_aot += modules[m].aot_count;
    }

    xcgen_rebuild_field_index(&struct_reg);

    // Phase 4: Build cross-module export map for import resolution
    int import_map_count = 0;
    XirAotImportEntry *import_map = aot_build_export_map(
        modules, nmodules, entry_index, &import_map_count);
    if (import_map_count > 0) {
        printf("[native] Cross-module exports: %d entries\n", import_map_count);
    }

    // Phase 5: Lower each module to XIR → C
    for (int m = 0; m < nmodules; m++) {
        if (nmodules > 1)
            printf("[native] Module: %s (%d functions)\n",
                   modules[m].mod_name, modules[m].aot_count);

        for (int i = 0; i < modules[m].aot_count; i++) {
            XrProto *p = modules[m].aot_protos[i];
            const char *fname = p->name ? XR_STRING_CHARS(p->name) : "__module_init";

            // Use extended builder with import/export opts for init functions
            XirFunc *xfunc;
            bool is_init = (p->name == NULL);
            bool needs_opts = is_init && (import_map_count > 0 ||
                                           modules[m].export_slot_count > 0);
            if (needs_opts) {
                XirAotOptions opts = {
                    .import_map = import_map,
                    .import_count = import_map_count,
                    .export_slots = modules[m].export_slots,
                    .export_slot_count = modules[m].export_slot_count,
                };
                xfunc = xir_build_from_proto_aot_ex(
                    p, shared_protos, nshared, X, &opts);
            } else {
                xfunc = xir_build_from_proto_aot(p, shared_protos, nshared, X);
            }

            if (!xfunc) {
                printf("  [C] %s → skip (XIR build failed)\n", fname);
                continue;
            }
            xir_run_pipeline(xfunc, XIR_OPT_FULL);
            XcgenFunc *cf = xcgen_compile_func(modules[m].cmod, xfunc,
                                                modules[m].aot_c_names[i]);
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

    printf("AOT: %d/%d functions transpiled across %d modules\n",
           total_compiled, total_aot, nmodules);

    // Phase 6: Emit C source + main() + link
    char *aot_source = (total_compiled > 0) ? xcgen_emit_source(comp) : NULL;
    xcgen_compilation_free(comp);
    comp = NULL;

    char c_file[512];
    if (c_only) snprintf(c_file, sizeof(c_file), "%s", output);
    else snprintf(c_file, sizeof(c_file), "/tmp/xray_native_%d.c", getpid());

    FILE *f = fopen(c_file, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", c_file);
        goto fail_final;
    }

    fprintf(f,
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#include <stdint.h>\n#include <stddef.h>\n\n");
    if (aot_source) {
        fprintf(f, "/* --- AOT Transpiled Functions --- */\n%s\n\n", aot_source);
        xr_free(aot_source); aot_source = NULL;
    }
    aot_write_main(f, modules, nmodules);
    fclose(f);

    // Free module info and import map
    for (int m = 0; m < nmodules; m++) {
        xr_free((void *)modules[m].path);
        xr_free((void *)modules[m].mod_name);
        xr_free(modules[m].aot_protos);
        xr_free(modules[m].aot_c_names);
        xr_free(modules[m].export_slots);
    }
    xr_free(modules);
    // Free import_map strings (module_path pointers are shared per-module group)
    if (import_map) {
        const char *prev = NULL;
        for (int i = 0; i < import_map_count; i++) {
            if (import_map[i].module_path != prev) {
                xr_free((void *)import_map[i].module_path);
                prev = import_map[i].module_path;
            }
        }
        xr_free(import_map);
    }

    if (c_only) { printf("Generated: %s\n", output); return 0; }

    int ret = invoke_cc_standalone(cc, opt_flag, output, c_file, strip, sysroot);
    unlink(c_file);
    if (ret == 0) printf("Generated: %s\n", output);
    return ret;

    // Error cleanup paths
fail_comp:
    xcgen_compilation_free(comp);
fail_isolate:
    xray_isolate_delete(X);
fail_early:
    for (int m = 0; m < nmodules; m++) {
        xr_free((void *)modules[m].path);
        xr_free((void *)modules[m].mod_name);
        xr_free(modules[m].aot_protos);
        xr_free(modules[m].aot_c_names);
        xr_free(modules[m].export_slots);
    }
    xr_free(modules);
    return 1;

fail_final:
    if (aot_source) xr_free(aot_source);
    for (int m = 0; m < nmodules; m++) {
        xr_free((void *)modules[m].path);
        xr_free((void *)modules[m].mod_name);
        xr_free(modules[m].aot_protos);
        xr_free(modules[m].aot_c_names);
        xr_free(modules[m].export_slots);
    }
    xr_free(modules);
    return 1;
}

#else

static int cmd_build_native(const char *input, const char *output,
                            const char *cc, const char *opt_flag,
                            bool c_only, bool strip, const char *sysroot) {
    (void)input; (void)output; (void)cc; (void)opt_flag; (void)c_only; (void)strip; (void)sysroot;
    fprintf(stderr, "Error: --native requires JIT support\n");
    return 1;
}

#endif
