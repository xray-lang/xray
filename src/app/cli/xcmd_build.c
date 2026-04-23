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
#include "xcli_utils.h"
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <getopt.h>
#include <spawn.h>
#include <sys/wait.h>
#include "../../base/xmalloc.h"

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

/* ========== Help ========== */

void print_build_help(void) {
    printf("Usage: xray build [options] <file.xr>\n\n");
    printf("Compile Xray script into executable.\n\n");
    printf("Options:\n");
    printf("    -o, --output <name>  Output file name (default: a.out)\n");
    printf("    -c                   Output C source only, no linking\n");
    printf("    --native             Native binary (AOT + runtime, like Go)\n");
    printf("    --cc <compiler>      C compiler (default: cc)\n");
    printf("    -O <level>           Optimization level: 0,1,2,3,s,fast (default: 2)\n");
    printf("    --sysroot <path>     Cross-compile sysroot path\n");
    printf("    --strip              Strip symbols\n");
    printf("    -h, --help           Show this help\n\n");
    printf("Build modes:\n");
    printf("    (default)      Bytecode bundle — full features, needs libxray_core\n");
    printf("    --native       AOT native — full features, static linked, like Go\n\n");
    printf("Examples:\n");
    printf("    xray build app.xr                  # bytecode bundle\n");
    printf("    xray build app.xr --native         # native + runtime (like Go)\n");
    printf("    xray build app.xr --native -c      # output C source only\n");
    printf("    xray build app.xr --native --cc gcc -O 3\n\n");
}

/* ========== Argument Parsing ========== */

static struct option build_long_options[] = {
    {"output",   required_argument, 0, 'o'},
    {"cc",       required_argument, 0, 'C'},
    {"sysroot",  required_argument, 0, 'r'},
    {"strip",    no_argument,       0, 'S'},
    {"native",   no_argument,       0, 'N'},
    {"help",     no_argument,       0, 'h'},
    {0, 0, 0, 0}
};

// Validate and format -O flag (returns string literal, no static buffer)
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

// Forward declarations
static int cmd_build_bytecode(const char *input, const char *output,
                              const char *cc, const char *opt_flag,
                              bool c_only, bool strip, const char *sysroot);
static int cmd_build_native(const char *input, const char *output,
                            const char *cc, const char *opt_flag,
                            bool c_only, bool strip, const char *sysroot);

int cmd_build(int argc, char **argv) {
    const char *input_file = NULL;
    const char *output_file = NULL;
    const char *cc = "cc";
    const char *opt_level = NULL;
    bool c_only = false;
    bool strip_symbols = false;
    bool native_mode = false;
    const char *sysroot = NULL;

    optind = 1;
    int opt;
    while ((opt = getopt_long(argc, argv, "o:cshC:r:O:SN", build_long_options, NULL)) != -1) {
        switch (opt) {
            case 'o': output_file = optarg; break;
            case 'c': c_only = true; break;
            case 'C': cc = optarg; break;
            case 'O': opt_level = optarg; break;
            case 'r': sysroot = optarg; break;
            case 'S': strip_symbols = true; break;
            case 'N': native_mode = true; break;
            case 'h': print_build_help(); return 0;
            default:  print_build_help(); return 1;
        }
    }

    if (optind < argc) input_file = argv[optind];
    if (!input_file) {
        fprintf(stderr, "Error: no input file\n");
        print_build_help();
        return 1;
    }
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

    XrayIsolate *X = cli_create_isolate();
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
static void collect_exports(XrProto *proto, XcgenModule *mod) {
    if (!proto || !mod) return;
    uint32_t code_count = (uint32_t)proto->code.count;
    const XrInstruction *code = (const XrInstruction *)proto->code.data;

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
        // Patterns:
        //   (a) OP_GETSHARED R[reg], idx → OP_EXPORT K[n], R[reg], c
        //   (b) OP_SETSHARED R[reg], idx → OP_EXPORT K[n], R[reg], c
        //   (c) OP_MOVE R[reg], R[src]   → trace src register
        int shared_idx = -1;
        int trace_reg = value_reg;
        for (int bpc = (int)pc - 1; bpc >= 0 && shared_idx < 0; bpc--) {
            XrInstruction prev = code[bpc];
            OpCode prev_op = GET_OPCODE(prev);
            if ((prev_op == OP_GETSHARED || prev_op == OP_SETSHARED) &&
                GETARG_A(prev) == trace_reg) {
                shared_idx = GETARG_Bx(prev) + proto->shared_offset;
            } else if (prev_op == OP_MOVE && GETARG_A(prev) == trace_reg) {
                // OP_MOVE R[A] = R[B] → trace to source register
                trace_reg = GETARG_B(prev);
            }
        }

        if (shared_idx >= 0) {
            xcgen_module_add_export(mod, export_name, shared_idx, is_const != 0);
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

/* ========== Native + Runtime (--native, AOT + bytecode hybrid) ========== */

static int cmd_build_native(const char *input, const char *output,
                            const char *cc, const char *opt_flag,
                            bool c_only, bool strip, const char *sysroot) {
    printf("[native] Building: %s (AOT + runtime)\n", input);

    // Phase 1: Create bytecode bundle + collect module paths
    char *bc_source = NULL;
    int bundle_module_count = 0;
    {
        XrayIsolate *Xb = cli_create_isolate();
        if (!Xb) { fprintf(stderr, "Error: failed to create isolate for bundling\n"); return 1; }

        XrBundle *bundle = xr_bundle_create_ex(Xb, input, XR_BUNDLE_DEFAULT);
        if (!bundle) {
            fprintf(stderr, "Error: bytecode bundling failed\n");
            xray_isolate_delete(Xb);
            return 1;
        }
        bc_source = xr_bundle_to_c_source(bundle, "xr_app");
        bundle_module_count = bundle->count;

        // Log discovered modules
        if (bundle->count > 1) {
            printf("[native] Bundle: %d modules\n", bundle->count);
            for (int i = 0; i < bundle->count; i++) {
                printf("  [%d] %s%s\n", i, bundle->entries[i].path,
                       (bundle->entry_path &&
                        strcmp(bundle->entries[i].path, bundle->entry_path) == 0)
                       ? " (entry)" : "");
            }
        }

        xr_bundle_free(bundle);
        xray_isolate_delete(Xb);
    }
    if (!bc_source) {
        fprintf(stderr, "Error: C source generation for bytecode failed\n");
        return 1;
    }

    // Phase 2: AOT compile typed functions (separate isolate)
    char *source = cli_read_file(input);
    if (!source) { fprintf(stderr, "Error: cannot open '%s'\n", input); xr_free(bc_source); return 1; }

    XrayIsolate *X = cli_create_isolate();
    if (!X) { xr_free(source); xr_free(bc_source); fprintf(stderr, "Error: failed to create isolate\n"); return 1; }

    XrProto *proto = xr_compile_source_with_path(X, source, input);
    xr_free(source);
    if (!proto) {
        fprintf(stderr, "Error: compilation failed\n");
        xray_isolate_delete(X);
        xr_free(bc_source);
        return 1;
    }

    // Collect AOT-eligible functions (dynamic array, no hard limit)
    int aot_cap = 128;
    XrProto **aot_protos = (XrProto **)xr_malloc(aot_cap * sizeof(XrProto *));
    if (!aot_protos) {
        fprintf(stderr, "Error: out of memory\n");
        xray_isolate_delete(X); xr_free(bc_source); return 1;
    }
    int aot_count = 0;
    collect_aot_protos(proto, aot_protos, &aot_count, aot_cap);

    // Create struct promotion registry
    XcgenStructRegistry struct_reg;
    xcgen_struct_registry_init(&struct_reg);
    xcgen_collect_shapes(proto, &struct_reg, (void *)X);
    for (int i = 0; i < aot_count; i++)
        xcgen_collect_shapes(aot_protos[i], &struct_reg, (void *)X);
    xcgen_rebuild_field_index(&struct_reg);

    // Create compilation context
    XcgenCompilation *comp = xcgen_compilation_new();
    comp->struct_reg = &struct_reg;

    // Add main module to compilation
    XcgenModule *mod = xcgen_compilation_add_module(comp, "main", input);

    // Collect module-level exports (scan OP_EXPORT + OP_SETSHARED pairs)
    collect_exports(proto, mod);

    // Allocate C name storage (dynamic, same count as aot_protos)
    char (*aot_c_names)[140] = (char (*)[140])xr_malloc(aot_count * 140);
    if (!aot_c_names) {
        fprintf(stderr, "Error: out of memory\n");
        xcgen_compilation_free(comp); xr_free(aot_protos);
        xray_isolate_delete(X); xr_free(bc_source); return 1;
    }

    // Register AOT proto→name mappings
    for (int i = 0; i < aot_count; i++) {
        XrProto *p = aot_protos[i];
        const char *name = p->name ? XR_STRING_CHARS(p->name) : "__module_init";
        snprintf(aot_c_names[i], 140, "xr_%s", name);
        for (int j = 0; j < i; j++) {
            if (strcmp(aot_c_names[i], aot_c_names[j]) == 0) {
                snprintf(aot_c_names[i], 140, "xr_%s_%d", name, i);
                break;
            }
        }
        xcgen_register_proto(comp, (void *)p, aot_c_names[i]);
    }

    // Build shared_index → proto mapping
    XrProto *shared_protos[128];
    bool shared_is_ctor[128];
    memset(shared_protos, 0, sizeof(shared_protos));
    memset(shared_is_ctor, 0, sizeof(shared_is_ctor));
    int nshared = build_shared_proto_map(proto, shared_protos, shared_is_ctor, 128);

    // Pre-register classes so CHA devirt resolves user-defined methods
    aot_preregister_classes(proto, X);

    int total_compiled = 0;

    for (int i = 0; i < aot_count; i++) {
        XrProto *p = aot_protos[i];
        const char *name = p->name ? XR_STRING_CHARS(p->name) : "__module_init";

        XirFunc *xfunc = xir_build_from_proto_aot(p, shared_protos, nshared, X);
        if (!xfunc) {
            printf("  [C] %s → skip (XIR build failed)\n", name);
            continue;
        }
        xir_run_pipeline(xfunc, XIR_OPT_FULL);
        XcgenFunc *cf = xcgen_compile_func(mod, xfunc, aot_c_names[i]);
        xir_func_destroy(xfunc);

        if (cf) {
            printf("  [C] %s → %zu bytes C source\n", name, cf->body.len);
            total_compiled++;
        } else {
            printf("  [C] %s → skip (xcgen failed)\n", name);
        }
    }

    xray_isolate_delete(X);

    printf("AOT: %d/%d functions transpiled (entry module, %d total modules in bundle)\n",
           total_compiled, aot_count, bundle_module_count);

    // Assemble AOT C source (without main)
    char *aot_source = NULL;
    if (total_compiled > 0) {
        aot_source = xcgen_emit_source(comp);
    }
    xcgen_compilation_free(comp);
    xr_free(aot_protos);
    xr_free(aot_c_names);

    // Write standalone AOT C source: functions + main()
    char c_file[512];
    if (c_only) snprintf(c_file, sizeof(c_file), "%s", output);
    else snprintf(c_file, sizeof(c_file), "/tmp/xray_native_rt_%d.c", getpid());

    FILE *f = fopen(c_file, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot create '%s'\n", c_file);
        xr_free(bc_source);
        if (aot_source) xr_free(aot_source);
        return 1;
    }

    // Headers
    fprintf(f,
        "#include <stdio.h>\n"
        "#include <stdlib.h>\n"
        "#include <string.h>\n"
        "#include <stdint.h>\n"
        "#include <stddef.h>\n\n"
    );

    // AOT transpiled functions
    if (aot_source) {
        fprintf(f, "/* --- AOT Transpiled Functions --- */\n");
        fprintf(f, "%s\n\n", aot_source);
        xr_free(aot_source);
    }
    xr_free(bc_source);

    // Standalone main: call __module_init directly (no VM)
    fprintf(f,
        "/* --- Standalone Main (no VM) --- */\n"
        "\nint main(int argc, char **argv) {\n"
        "    (void)argc; (void)argv;\n"
        "    xr___module_init(NULL);\n"
        "    return 0;\n"
        "}\n"
    );
    fclose(f);

    if (c_only) {
        printf("Generated: %s\n", output);
        return 0;
    }

    // Link standalone: only libc + libm (no libxray_core)
    int ret = invoke_cc_standalone(cc, opt_flag, output, c_file, strip, sysroot);
    unlink(c_file);
    if (ret == 0) printf("Generated: %s\n", output);
    return ret;
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
