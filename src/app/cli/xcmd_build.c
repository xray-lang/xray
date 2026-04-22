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

// Build shared_index → child_proto mapping by scanning top-level bytecode
// Pattern: CLOSURE R[A], Proto[Bx]; SETSHARED R[A], G[j]
// Returns max shared index + 1 (size of shared_protos array)
static int build_shared_proto_map(XrProto *top, XrProto **shared_protos, int max_shared) {
    if (!top) return 0;
    int max_idx = 0;
    uint32_t code_count = (uint32_t)top->code.count;
    const XrInstruction *code = (const XrInstruction *)top->code.data;
    for (uint32_t pc = 0; pc + 1 < code_count; pc++) {
        XrInstruction inst0 = code[pc];
        XrInstruction inst1 = code[pc + 1];
        OpCode op0 = GET_OPCODE(inst0);
        OpCode op1 = GET_OPCODE(inst1);

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
    }
    return max_idx;
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
    memset(shared_protos, 0, sizeof(shared_protos));
    int nshared = build_shared_proto_map(proto, shared_protos, 128);

    // Track compiled AOT functions for thunk/registration generation
    typedef struct {
        char xr_name[64];    // original xray function name
        char c_name[140];    // C function name (e.g. "xr_add")
        int nparams;
        uint8_t param_types[8];
        uint8_t ret_type;
        bool needs_closure;
        bool void_return;
    } AotFuncInfo;
    int compiled_cap = aot_count > 0 ? aot_count : 16;
    AotFuncInfo *compiled_funcs = (AotFuncInfo *)xr_malloc(
        compiled_cap * sizeof(AotFuncInfo));
    int ncompiled = 0;

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

        // Read types directly from proto (no lossy round-trip through XrSlotType).
        // proto->param_types and return_type_info are the authoritative type sources.
        uint8_t xir_ptypes[8] = {0};
        uint8_t xir_rtype = p->return_type_info
            ? xr_type_to_slot_type(p->return_type_info) : XR_SLOT_ANY;
        if (p->param_types) {
            for (int j = 0; j < p->numparams && j < 8; j++) {
                if (j < p->param_types_count && p->param_types[j])
                    xir_ptypes[j] = xr_type_to_slot_type(p->param_types[j]);
            }
        }
        xir_func_destroy(xfunc);

        if (cf && ncompiled < compiled_cap) {
            printf("  [C] %s → %zu bytes C source\n", name, cf->body.len);
            // Functions that create child closures (have sub-protos) cannot
            // register AOT thunks: xrt_closure_new creates AOT closures
            // incompatible with VM's GC-managed XrClosure objects.
            // Leaf closure functions (upvalue access only) are safe.
            if (p->protos.count > 0) {
                printf("  [C] %s → skip AOT hot path (creates child closures)\n", name);
            } else {
                AotFuncInfo *info = &compiled_funcs[ncompiled];
                snprintf(info->xr_name, sizeof(info->xr_name), "%s", name);
                snprintf(info->c_name, sizeof(info->c_name), "%s", aot_c_names[i]);
                info->nparams = p->numparams;
                info->ret_type = xir_rtype;
                info->needs_closure = cf->needs_closure_param;
                info->void_return = cf->void_return;
                // Use XIR-inferred types: matches actual C function signature
                memcpy(info->param_types, xir_ptypes, 8);
                ncompiled++;
            }
        } else if (!cf) {
            printf("  [C] %s → skip (xcgen failed)\n", name);
        }
    }

    xray_isolate_delete(X);

    printf("AOT: %d/%d functions transpiled (entry module, %d total modules in bundle)\n",
           ncompiled, aot_count, bundle_module_count);

    // Assemble AOT C source (without main)
    char *aot_source = NULL;
    if (ncompiled > 0) {
        aot_source = xcgen_emit_source(comp);
    }
    xcgen_compilation_free(comp);
    xr_free(aot_protos);
    xr_free(aot_c_names);

    // Write combined C source: bytecode bundle + AOT functions + thunks + main()
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

    // Bytecode bundle data
    fprintf(f, "/* --- Bytecode Bundle --- */\n");
    fprintf(f, "%s\n\n", bc_source);
    xr_free(bc_source);

    // AOT transpiled functions (if any)
    if (aot_source) {
        fprintf(f, "/* --- AOT Transpiled Functions --- */\n");
        fprintf(f, "%s\n\n", aot_source);
        xr_free(aot_source);
    }

    // Generate JIT-convention thunks for each AOT function
    if (ncompiled > 0) {
        fprintf(f, "/* --- AOT Thunks (JIT calling convention adapters) --- */\n\n");
        for (int i = 0; i < ncompiled; i++) {
            AotFuncInfo *info = &compiled_funcs[i];
            bool ret_is_tagged = (info->ret_type == XR_SLOT_PTR || info->ret_type == XR_SLOT_ANY);

            // Thunk: int64_t name_thunk(intptr_t coro, int64_t *args)
            fprintf(f, "static int64_t %s_thunk(intptr_t coro, int64_t *args) {\n", info->c_name);

            // Determine return category
            bool ret_is_float = (info->ret_type == XR_SLOT_F64);
            bool ret_is_void = info->void_return;

            // Build call expression with appropriate return handling
            fprintf(f, "    ");
            if (ret_is_void)
                fprintf(f, "%s(", info->c_name);
            else if (ret_is_tagged)
                fprintf(f, "XrtValue r = %s(", info->c_name);
            else if (ret_is_float)
                fprintf(f, "double rd = %s(", info->c_name);
            else
                fprintf(f, "return %s(", info->c_name);

            // Pass coro as XrtContext (first implicit parameter)
            // Native mode: closure upvalues accessed via xrt_ctx, no separate closure param
            fprintf(f, "(void*)coro");
            bool first = false;
            int arg_idx = 0;
            for (int j = 0; j < info->nparams; j++) {
                if (!first) fprintf(f, ", ");
                first = false;
                bool param_float = (info->param_types[j] == XR_SLOT_F64);
                bool param_tagged = (info->param_types[j] == XR_SLOT_PTR ||
                                     info->param_types[j] == XR_SLOT_ANY);
                if (param_float) {
                    fprintf(f, "*(double*)&args[%d]", arg_idx);
                } else if (param_tagged) {
                    fprintf(f, "((XrtValue*)&args[%d])[0]", arg_idx);
                    arg_idx++;  // tagged values use 2 slots in raw_args
                } else {
                    fprintf(f, "args[%d]", arg_idx);
                }
                arg_idx++;
            }
            fprintf(f, ");\n");

            if (ret_is_void) {
                fprintf(f, "    return 0;\n");
            } else if (ret_is_tagged) {
                fprintf(f, "    return r.i;\n");
            } else if (ret_is_float) {
                // Preserve IEEE754 bit pattern: double → int64_t via memcpy
                fprintf(f, "    int64_t ri; memcpy(&ri, &rd, 8); return ri;\n");
            }
            fprintf(f, "}\n\n");
        }
    }

    // Generate AOT registration and main
    fprintf(f, "/* --- AOT Registration & Main --- */\n");
    fprintf(f, "#include \"xray_isolate.h\"\n");
    fprintf(f, "extern void xr_multicore_init(void*, int);\n");
    fprintf(f, "extern void xr_multicore_destroy(void*);\n");

    if (ncompiled > 0) {
        // Decomposed bytecode API: deserialize → register AOT → execute → free
        fprintf(f,
            "\nstruct XrProto;\n"
            "extern struct XrProto* xr_bytecode_load(void*, const uint8_t*, size_t);\n"
            "extern int xr_execute(void*, struct XrProto*);\n"
            "extern void xr_vm_proto_free(struct XrProto*);\n"
            "\n"
            "/* Proto tree traversal for AOT thunk registration */\n"
            "extern const char* xr_proto_name(struct XrProto*);\n"
            "extern struct XrProto** xr_proto_children(struct XrProto*, int*);\n"
            "extern void xr_proto_set_jit_entry(struct XrProto*, void*);\n"
            "extern void xr_proto_set_param_types(struct XrProto*, const uint8_t*, int, uint8_t);\n"
            "\n"
            "typedef struct {\n"
            "    const char *name;\n"
            "    void *thunk;\n"
            "    const uint8_t *param_types;\n"
            "    int nparams;\n"
            "    uint8_t ret_type;\n"
            "} AotEntry;\n"
            "\n"
            "static void register_aot_in_proto(struct XrProto *p,\n"
            "    const AotEntry *entries, int count) {\n"
            "    if (!p) return;\n"
            "    const char *pname = xr_proto_name(p);\n"
            "    if (pname) {\n"
            "        for (int i = 0; i < count; i++) {\n"
            "            if (entries[i].name && strcmp(pname, entries[i].name) == 0) {\n"
            "                xr_proto_set_jit_entry(p, entries[i].thunk);\n"
            "                xr_proto_set_param_types(p, entries[i].param_types,\n"
            "                    entries[i].nparams, entries[i].ret_type);\n"
            "            }\n"
            "        }\n"
            "    }\n"
            "    int nchildren = 0;\n"
            "    struct XrProto **children = xr_proto_children(p, &nchildren);\n"
            "    for (int i = 0; i < nchildren; i++)\n"
            "        register_aot_in_proto(children[i], entries, count);\n"
            "}\n"
        );
    } else {
        fprintf(f, "extern int xr_eval_bytecode(void*, const uint8_t*, size_t);\n");
    }

    // Main function
    fprintf(f,
        "\nint main(int argc, char **argv) {\n"
        "    XrayIsolateParams params;\n"
        "    xray_isolate_params_init(&params);\n"
        "    xray_isolate_setup_full(&params);\n"
        "    XrayIsolate *X = xray_isolate_new(&params);\n"
        "    if (!X) { fprintf(stderr, \"Failed to create runtime\\n\"); return 1; }\n"
        "    xr_multicore_init(X, 0);\n"
        "    xray_isolate_set_script_info(X, argv[0], argc > 1 ? argc - 1 : 0, argc > 1 ? argv + 1 : NULL);\n"
        "    const XrEmbeddedModule *entry = &xr_app_modules[xr_app_entry_index];\n"
    );

    if (ncompiled > 0) {
        // Decomposed execution: deserialize → register AOT thunks → execute → free
        fprintf(f,
            "\n    /* Deserialize bytecode into proto tree */\n"
            "    struct XrProto *proto = xr_bytecode_load(X, entry->bc, entry->size);\n"
            "    if (!proto) { fprintf(stderr, \"Failed to load bytecode\\n\"); return 1; }\n"
        );

        fprintf(f, "\n    /* Register AOT thunks into proto tree */\n");

        // Emit param_types arrays for each AOT function
        for (int i = 0; i < ncompiled; i++) {
            AotFuncInfo *info = &compiled_funcs[i];
            if (info->nparams > 0) {
                fprintf(f, "    static const uint8_t %s_ptypes[] = {", info->c_name);
                for (int j = 0; j < info->nparams; j++)
                    fprintf(f, "%s%d", j ? "," : "", info->param_types[j]);
                fprintf(f, "};\n");
            }
        }

        // Emit AotEntry table
        fprintf(f, "    static const AotEntry aot_entries[] = {\n");
        for (int i = 0; i < ncompiled; i++) {
            AotFuncInfo *info = &compiled_funcs[i];
            fprintf(f, "        {\"%s\", (void*)%s_thunk, ", info->xr_name, info->c_name);
            if (info->nparams > 0)
                fprintf(f, "%s_ptypes", info->c_name);
            else
                fprintf(f, "NULL");
            fprintf(f, ", %d, %d}", info->nparams, info->ret_type);
            fprintf(f, "%s\n", i < ncompiled - 1 ? "," : "");
        }
        fprintf(f, "    };\n");
        fprintf(f, "    register_aot_in_proto(proto, aot_entries, %d);\n", ncompiled);

        fprintf(f,
            "\n    /* Execute with AOT hot paths active */\n"
            "    int result = xr_execute(X, proto);\n"
            "    xr_vm_proto_free(proto);\n"
        );
    } else {
        fprintf(f,
            "    int result = xr_eval_bytecode(X, entry->bc, entry->size);\n"
        );
    }

    fprintf(f,
        "    xr_multicore_destroy(X);\n"
        "    xray_isolate_delete(X);\n"
        "    return result;\n"
        "}\n"
    );
    fclose(f);

    xr_free(compiled_funcs);

    if (c_only) {
        printf("Generated: %s\n", output);
        return 0;
    }

    // Link against xray runtime
    int ret = invoke_cc(cc, opt_flag, output, c_file, NULL, strip, sysroot);
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
