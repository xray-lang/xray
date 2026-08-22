/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmodule.c - Module system core implementation
 *
 * KEY CONCEPT:
 *   Module loading, caching, path resolution, and export management.
 */

#include "xmodule.h"
#include "xmodule_diagnostic.h"
#include "xray_vm.h"
#include "../stdlib/xstdlib_vm_fastpath.h"
#include "xmodule_graph.h"
#include "xmodule_resolver.h"
#include "xproject.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xvm.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include "../base/xmalloc.h"
#include "../base/xfileio.h"
#include "../runtime/xerror.h"
#include "../runtime/mem/xheap.h"
#include "../toolchain/xcompiler_session.h"
#include "../base/xhashmap.h"
#include "../runtime/xray_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "../os/os_fs.h"
#include "../os/os_proc.h"

/* ========== Forward Declarations ========== */

#include "xproto_codec.h"

#include "xstdlib_embedded.h"

// xr_vm_execute_module declared in vm/xvm.h (included via xisolate_internal.h → xvm_state.h → ...)

void xr_module_set_compiler_hooks(XrVMRuntime *isolate, XrCompilerSession *compiler_session,
                                  XrModuleParseHook parse_fn, XrModuleCompileAstHook compile_ast_fn,
                                  XrModuleCompileSourceHook compile_src_fn,
                                  XrModuleAstFreeHook ast_free_fn) {
    XrModuleRegistry *registry = (XrModuleRegistry *) xr_isolate_get_module_registry(isolate);
    /* Module system may legitimately not be initialised on isolates that
     * do not import code (e.g. transient analyzer-only isolates), so this
     * is a graceful no-op rather than an assertion. */
    if (!registry)
        return;
    registry->compiler_session = compiler_session;
    registry->fn_parse = parse_fn;
    registry->fn_compile_ast = compile_ast_fn;
    registry->fn_compile_src = compile_src_fn;
    registry->fn_ast_free = ast_free_fn;
}

/* ========== Helper Functions ========== */

/*
** Normalize path
** Remove "." and resolve ".." segments.
** E.g. "a/b/./c" -> "a/b/c", "a/b/../c" -> "a/c"
** Does NOT touch the filesystem (purely lexical).
*/
static char *normalize_path(const char *path) {
    if (!path)
        return NULL;

    size_t len = strlen(path);
    char *buf = (char *) xr_malloc(len + 1);
    if (!buf)
        return NULL;
    memcpy(buf, path, len + 1);

    // Split into components and resolve in place using a stack of offsets
    // Stack stores start-offsets of kept components inside buf
    int stack[256];
    int top = 0;
    bool absolute = (buf[0] == '/');

    char *p = buf;
    while (*p) {
        // Skip leading slashes
        while (*p == '/')
            p++;
        if (!*p)
            break;

        // Find end of component
        char *comp = p;
        while (*p && *p != '/')
            p++;
        size_t clen = (size_t) (p - comp);

        if (clen == 1 && comp[0] == '.') {
            // "." — skip
            continue;
        }
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            // ".." — pop if possible (don't pop past root for absolute paths)
            if (top > 0) {
                top--;
            }
            continue;
        }
        // Push component
        XR_DCHECK(top < 256, "normalize_path: path too deep");
        if (top < 256) {
            char saved = *p;
            comp[clen] = '\0';  // NUL-terminate component
            stack[top++] = (int) (comp - buf);
            if (saved == '/')
                p++;
        }
    }

    // Rebuild path
    char *result = (char *) xr_malloc(len + 1);
    if (!result) {
        xr_free(buf);
        return NULL;
    }
    char *dst = result;

    if (absolute)
        *dst++ = '/';

    for (int i = 0; i < top; i++) {
        if (i > 0)
            *dst++ = '/';
        const char *comp = buf + stack[i];
        size_t clen = strlen(comp);
        memcpy(dst, comp, clen);
        dst += clen;
    }
    *dst = '\0';

    // Empty result means current dir
    if (result[0] == '\0') {
        result[0] = '.';
        result[1] = '\0';
    }

    xr_free(buf);
    return result;
}

/* ========== Module Object Operations ========== */

/*
** Initialize export table fields to zero
*/
static void module_init_exports(XrModule *module) {
    module->export_values = NULL;
    module->export_symbols = NULL;
    module->export_flags = NULL;
    module->symbol_to_index = NULL;
    module->min_symbol = 0;
    module->max_symbol = 0;
    module->export_count = 0;
    module->export_capacity = 0;
}

XrModuleState xr_module_state(const XrModule *module) {
    if (!module)
        return XR_MODULE_FAILED;
    return (XrModuleState) atomic_load_explicit(&module->state, memory_order_acquire);
}

bool xr_module_begin_initialization(XrModule *module) {
    if (!module)
        return false;
    uint8_t expected = XR_MODULE_NEW;
    return atomic_compare_exchange_strong_explicit(&module->state, &expected,
                                                   XR_MODULE_INITIALIZING, memory_order_acq_rel,
                                                   memory_order_acquire);
}

bool xr_module_publish(XrModule *module) {
    if (!module || xr_module_state(module) != XR_MODULE_INITIALIZING)
        return false;
    xr_module_build_export_index(module);
    atomic_store_explicit(&module->state, XR_MODULE_PUBLISHED, memory_order_release);
    return true;
}

void xr_module_fail(XrModule *module) {
    if (!module)
        return;
    atomic_store_explicit(&module->state, XR_MODULE_FAILED, memory_order_release);
}

/*
** Create Native module
*/
XrModule *xr_module_create_native(XrVMRuntime *isolate, const char *name) {
    XR_DCHECK(isolate != NULL, "module_create_native: NULL isolate");
    XR_DCHECK(name != NULL, "module_create_native: NULL name");
    XrModule *module = (XrModule *) xr_fixed_heap_alloc(xr_isolate_get_fixed_heap(isolate),
                                                        sizeof(XrModule), XR_TMODULE);
    xr_obj_header_init_type(&module->hdr, XR_TMODULE);

    module->name = xr_strdup(name);
    module->path = NULL;
    module->module_type = MODULE_TYPE_NATIVE;

    module_init_exports(module);

    atomic_init(&module->state, XR_MODULE_NEW);
    module->requires_script = false;
    module->native_handle = NULL;
    module->native_handle_destroy = NULL;
    module->init_fn = NULL;
    module->compiled_code = NULL;

    return module;
}

/*
** Create Script module
*/
XrModule *xr_module_create_script(XrVMRuntime *isolate, const char *name, const char *path) {
    XR_DCHECK(isolate != NULL, "module_create_script: NULL isolate");
    XR_DCHECK(name != NULL, "module_create_script: NULL name");
    XrModule *module = (XrModule *) xr_fixed_heap_alloc(xr_isolate_get_fixed_heap(isolate),
                                                        sizeof(XrModule), XR_TMODULE);
    xr_obj_header_init_type(&module->hdr, XR_TMODULE);

    module->name = xr_strdup(name);
    module->path = xr_strdup(path);
    module->module_type = MODULE_TYPE_SCRIPT;

    module_init_exports(module);

    atomic_init(&module->state, XR_MODULE_NEW);
    module->requires_script = false;
    module->native_handle = NULL;
    module->native_handle_destroy = NULL;
    module->init_fn = NULL;
    module->compiled_code = NULL;

    return module;
}

/*
** Add export by SymbolId (core function)
*/
void xr_module_add_export_sym(XrVMRuntime *isolate, XrModule *module, SymbolId sym, XrValue value,
                              bool is_const) {
    (void) isolate;
    if (!module)
        return;
    XrModuleState state = xr_module_state(module);
    if (state != XR_MODULE_NEW && state != XR_MODULE_INITIALIZING) {
        xr_log_warning("module", "cannot modify exports after module publication");
        return;
    }

    // Check if symbol already exists (update case)
    for (uint16_t i = 0; i < module->export_count; i++) {
        if (module->export_symbols[i] == sym) {
            module->export_values[i] = value;
            if (is_const)
                module->export_flags[i] |= XR_EXPORT_CONST;
            return;
        }
    }

    // Grow arrays if needed
    if (module->export_count >= module->export_capacity) {
        uint16_t new_cap = module->export_capacity ? module->export_capacity * 2 : 8;
        XR_REALLOC_OR_ABORT(module->export_values, (size_t) new_cap * sizeof(XrValue),
                            "module export_values grow");
        XR_REALLOC_OR_ABORT(module->export_symbols, (size_t) new_cap * sizeof(SymbolId),
                            "module export_symbols grow");
        XR_REALLOC_OR_ABORT(module->export_flags, (size_t) new_cap * sizeof(uint8_t),
                            "module export_flags grow");
        module->export_capacity = new_cap;
    }

    uint16_t idx = module->export_count++;
    XR_DCHECK(module->export_count <= module->export_capacity,
              "module_add_export: count > capacity");
    module->export_values[idx] = value;
    module->export_symbols[idx] = sym;
    module->export_flags[idx] = is_const ? XR_EXPORT_CONST : 0;

    // Invalidate sparse index (will be rebuilt)
    if (module->symbol_to_index) {
        xr_free(module->symbol_to_index);
        module->symbol_to_index = NULL;
    }
}

/*
** Add module export (string-based convenience wrapper)
** Resolves name to SymbolId internally — stdlib modules use this unchanged
*/
void xr_module_add_export(XrVMRuntime *isolate, XrModule *module, const char *name, XrValue value) {
    if (!isolate || !module || !name)
        return;

    XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_register_in_table(sym_table, name);
    xr_module_add_export_sym(isolate, module, sym, value, false);
}

bool xr_module_set_initializing_export(XrVMRuntime *isolate, XrModule *module, const char *name,
                                       XrValue value, bool is_const) {
    if (!isolate || !module || !name ||
        atomic_load_explicit(&module->state, memory_order_acquire) != XR_MODULE_INITIALIZING)
        return false;

    XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_register_in_table(sym_table, name);
    if (sym < 0)
        return false;
    for (uint16_t i = 0; i < module->export_count; i++) {
        if (module->export_symbols[i] != sym)
            continue;
        module->export_values[i] = value;
        if (module->export_flags)
            module->export_flags[i] = is_const ? XR_EXPORT_CONST : 0;
        return true;
    }
    xr_module_add_export_sym(isolate, module, sym, value, is_const);
    return true;
}

/*
** Get module export (string-based)
*/
XrValue xr_module_get_export(XrVMRuntime *isolate, XrModule *module, const char *name) {
    if (!isolate || !module || !name)
        return xr_null();

    XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_lookup_in_table(sym_table, name);
    if (sym < 0)
        return xr_null();
    return xr_module_get_sym(module, sym);
}

/*
** Build sparse SymbolId→index lookup table
** Called by xr_module_publish after all exports are added.
*/
void xr_module_build_export_index(XrModule *module) {
    if (!module || module->export_count == 0)
        return;

    // Free old index
    if (module->symbol_to_index) {
        xr_free(module->symbol_to_index);
        module->symbol_to_index = NULL;
    }

    // Find min/max symbol range
    SymbolId min_sym = module->export_symbols[0];
    SymbolId max_sym = module->export_symbols[0];
    for (uint16_t i = 1; i < module->export_count; i++) {
        if (module->export_symbols[i] < min_sym)
            min_sym = module->export_symbols[i];
        if (module->export_symbols[i] > max_sym)
            max_sym = module->export_symbols[i];
    }

    int range = max_sym - min_sym + 1;

    // Safety: if range is absurdly large, skip index (linear scan fallback)
    if (range > 4096)
        return;

    module->min_symbol = min_sym;
    module->max_symbol = max_sym;
    module->symbol_to_index = (int32_t *) xr_malloc(range * sizeof(int32_t));
    if (!module->symbol_to_index)
        return;
    memset(module->symbol_to_index, 0xFF, range * sizeof(int32_t));  // -1

    for (uint16_t i = 0; i < module->export_count; i++) {
        module->symbol_to_index[module->export_symbols[i] - min_sym] = (int32_t) i;
    }
}

/*
** Free module object
*/
void xr_module_free(XrModule *module) {
    if (!module)
        return;

    if (module->native_handle && module->native_handle_destroy) {
        module->native_handle_destroy(module->native_handle);
        module->native_handle = NULL;
    }

    if (module->name)
        xr_free(module->name);
    if (module->path)
        xr_free(module->path);

    // Free export arrays
    if (module->export_values)
        xr_free(module->export_values);
    if (module->export_symbols)
        xr_free(module->export_symbols);
    if (module->export_flags)
        xr_free(module->export_flags);
    if (module->symbol_to_index)
        xr_free(module->symbol_to_index);
    if (module->compiled_code) {
        xr_free(module->compiled_code);
        module->compiled_code = NULL;
    }
}

/* ========== Module System Initialization ========== */

/*
** Resolve the stdlib root directory. Pure-Xray stdlib modules
** (stdlib/<name>/<name>.xr script layers) are loaded from disk, so the
** path must resolve regardless of the process working directory:
**
**   1. XRAY_STDLIB_PATH environment override (must exist)
**   2. <exedir>/../stdlib          — development build tree (build/xray)
**   3. <exedir>/../lib/xray/stdlib — installed layout (bin/xray)
**   4. "stdlib" relative to cwd    — repo-root workflows / last resort
**
** Returns an xr_malloc'd string; caller owns it.
*/
static char *resolve_stdlib_root(void) {
    const char *env = getenv("XRAY_STDLIB_PATH");
    if (env && env[0] && xr_fs_exists(env))
        return xr_strdup(env);

    char exe[XR_PATH_MAX];
    if (xr_proc_self_exe_path(exe, sizeof(exe)) == 0) {
        char *dir = xr_path_dirname(exe);
        if (dir) {
            static const char *const suffixes[] = {"../stdlib", "../lib/xray/stdlib"};
            for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
                char cand[XR_PATH_MAX];
                snprintf(cand, sizeof(cand), "%s/%s", dir, suffixes[i]);
                if (xr_fs_exists(cand)) {
                    char *real = xr_realpath(cand);
                    xr_free(dir);
                    return real ? real : xr_strdup(cand);
                }
            }
            xr_free(dir);
        }
    }

    return xr_strdup("stdlib");
}

/*
** Create module registry
*/
static XrModuleRegistry *create_registry(void) {
    XrModuleRegistry *registry = (XrModuleRegistry *) xr_malloc(sizeof(XrModuleRegistry));
    if (!registry)
        return NULL;

    memset(registry, 0, sizeof(*registry));
    registry->native_factories = xr_hashmap_new();
    registry->loaded_modules = xr_hashmap_new();
    registry->stdlib_path = resolve_stdlib_root();

    return registry;
}

/*
** Destroy module registry
*/
static void destroy_registry(XrModuleRegistry *registry) {
    if (!registry)
        return;

    if (registry->native_factories) {
        xr_hashmap_free(registry->native_factories);
    }
    if (registry->loaded_modules) {
        // Free module-owned payloads before destroying the non-owning hashmap.
        for (uint32_t i = 0; i < registry->loaded_modules->capacity; i++) {
            XrHashMapEntry *entry = &registry->loaded_modules->entries[i];
            if (entry->key != NULL && entry->value != NULL) {
                xr_module_free((XrModule *) entry->value);
            }
        }
        xr_hashmap_free(registry->loaded_modules);
    }
    if (registry->stdlib_path) {
        xr_free(registry->stdlib_path);
    }

    // Free topo-ordered module table (pointers only; modules freed via loaded_modules)
    if (registry->module_table) {
        xr_free(registry->module_table);
    }

    // Free resolver
    if (registry->resolver) {
        xr_module_resolver_free(registry->resolver);
    }

    // Free project config
    if (registry->project) {
        xr_project_free(registry->project);
    }

    xr_free(registry);
}

/*
** Initialize module system
*/
void xr_module_system_init(XrVMRuntime *isolate) {
    if (!isolate)
        return;

    // Create module registry
    xr_isolate_set_module_registry(isolate, create_registry());

    if (!xr_isolate_get_module_registry(isolate)) {
        xr_log_warning("module", "failed to create module registry");
        return;
    }

    // Register standard library modules
    xr_module_register_stdlib(isolate);
}

/*
** Initialize module system (with script path)
** Load project config (if xray.toml exists)
*/
void xr_module_system_init_with_script(XrVMRuntime *isolate, const char *script_path) {
    if (!isolate)
        return;

    // Get or create module registry
    XrModuleRegistry *registry = (XrModuleRegistry *) xr_isolate_get_module_registry(isolate);
    if (!registry) {
        xr_isolate_set_module_registry(isolate, create_registry());
        registry = (XrModuleRegistry *) xr_isolate_get_module_registry(isolate);
        if (!registry) {
            xr_log_warning("module", "failed to create module registry");
            return;
        }
        // Register standard library modules (only on first creation)
        xr_module_register_stdlib(isolate);
    }

    // Try to load project config (for package management)
    if (script_path && !registry->project) {
        char *dir = xr_path_dirname(script_path);
        registry->project = xr_project_load(isolate, dir);
        xr_free(dir);
    }
}

/*
** Free module system
*/
void xr_module_system_free(XrVMRuntime *isolate) {
    if (!isolate || !xr_isolate_get_module_registry(isolate))
        return;

    destroy_registry((XrModuleRegistry *) xr_isolate_get_module_registry(isolate));
    xr_isolate_set_module_registry(isolate, NULL);
}

/* ========== Module Registration ========== */

/*
** Register a native module factory
*/
void xr_module_register_native_factory(XrVMRuntime *isolate, const char *name,
                                       XrNativeModuleFactory factory) {
    if (!isolate || !name || !factory) {
        xr_log_warning("module", "invalid arguments to register native factory");
        return;
    }

    XrModuleRegistry *registry = (XrModuleRegistry *) xr_isolate_get_module_registry(isolate);
    if (!registry) {
        xr_log_warning("module", "module registry not initialized");
        return;
    }

    if (!xr_hashmap_set(registry->native_factories, name, (void *) factory)) {
        xr_log_warning("module", "out of memory registering native factory '%s'", name);
    }
}

/* ========== Path Resolution ========== */

/*
 * Ensure the registry has a resolver instance.  Created lazily because
 * native_factories are populated after create_registry() returns.
 */
static XrModuleResolver *ensure_resolver(XrModuleRegistry *registry) {
    if (registry->resolver)
        return registry->resolver;

    XrModuleResolverConfig cfg = {
        .native_factories = registry->native_factories,
        .stdlib_path = registry->stdlib_path,
        .lockfile = NULL,
    };
    registry->resolver = xr_module_resolver_new(&cfg);
    return registry->resolver;
}

XR_FUNC XrModuleResolver *xr_module_registry_get_resolver(XrModuleRegistry *registry) {
    if (!registry)
        return NULL;
    return ensure_resolver(registry);
}

/*
 * Get the importer path from the isolate (current module path, or
 * entry script path). Returns NULL if neither is available.
 */
static const char *get_importer_path(XrVMRuntime *isolate) {
    XrModule *cur = xr_isolate_get_current_module(isolate);
    if (cur && cur->path)
        return cur->path;
    return xr_isolate_get_script_file(isolate);
}

/* Resolve a physical I/O locator to the graph-owned typed identity authority.
 * The physical path is used only to select a live graph node; it never becomes
 * durable identity text. Ambiguous or graphless lookups fail closed. */
static const XrModuleSpec *module_spec_for_locator(const XrModuleRegistry *registry,
                                                   const char *locator) {
    if (!registry || !registry->compiler_session || !locator || !locator[0])
        return NULL;
    const XrModuleGraph *graph =
        xr_compiler_session_module_graph(registry->compiler_session);
    if (!graph)
        return NULL;
    const XrModuleSpec *match = NULL;
    char *locator_realpath = xr_realpath(locator);
    for (int i = 0; i < graph->spec_count; i++) {
        const XrModuleSpec *spec = &graph->specs[i];
        if (!spec->source_path || !spec->canonical ||
            !xr_module_identity_authority_valid(&spec->authority))
            continue;
        bool matches = strcmp(spec->canonical, locator) == 0 ||
                       strcmp(spec->source_path, locator) == 0;
        if (!matches && locator_realpath) {
            char *spec_realpath = xr_realpath(spec->source_path);
            matches = spec_realpath && strcmp(spec_realpath, locator_realpath) == 0;
            xr_free(spec_realpath);
        }
        if (!matches)
            continue;
        if (match && strcmp(match->canonical, spec->canonical) != 0) {
            xr_free(locator_realpath);
            return NULL;
        }
        match = spec;
    }
    xr_free(locator_realpath);
    return match;
}

static const XrModuleIdentityAuthority *module_authority_for_source(
    const XrModuleRegistry *registry, const char *source_path) {
    const XrModuleSpec *spec = module_spec_for_locator(registry, source_path);
    return spec ? &spec->authority : NULL;
}

static const XrBytecodeModule *find_embedded_module(const XrModuleRegistry *registry,
                                                    const char *path) {
    if (!registry || !path)
        return NULL;
    for (size_t i = 0; i < registry->embedded_module_count; i++) {
        const XrBytecodeModule *module = &registry->embedded_modules[i];
        if (module->path && module->bytecode && module->bytecode_size > 0 &&
            strcmp(module->path, path) == 0)
            return module;
    }
    return NULL;
}

static char *resolve_embedded_relative(const XrModuleRegistry *registry, const char *specifier,
                                       const char *importer) {
    if (!registry || !specifier || !importer || specifier[0] != '.')
        return NULL;

    char *base_dir = xr_path_dirname(importer);
    if (!base_dir)
        return NULL;

    char candidate[XR_PATH_MAX];
    snprintf(candidate, sizeof(candidate), "%s/%s", base_dir, specifier);
    xr_free(base_dir);
    char *normalized = normalize_path(candidate);
    if (!normalized)
        return NULL;

    if (find_embedded_module(registry, normalized))
        return normalized;

    size_t length = strlen(normalized);
    if (length < 3 || strcmp(normalized + length - 3, ".xr") != 0) {
        snprintf(candidate, sizeof(candidate), "%s.xr", normalized);
        if (find_embedded_module(registry, candidate)) {
            xr_free(normalized);
            return xr_strdup(candidate);
        }
    }

    snprintf(candidate, sizeof(candidate), "%s/index.xr", normalized);
    if (find_embedded_module(registry, candidate)) {
        xr_free(normalized);
        return xr_strdup(candidate);
    }

    xr_free(normalized);
    return NULL;
}

/*
** Resolve module path
**
** Delegates to XrModuleResolver for script path resolution.
** Returns xr_malloc'd absolute path or NULL.
*/
char *xr_module_resolve_path(XrVMRuntime *isolate, const char *module_name) {
    if (!module_name || !isolate)
        return NULL;

    XrModuleRegistry *registry = (XrModuleRegistry *) xr_isolate_get_module_registry(isolate);
    if (!registry)
        return NULL;

    const char *importer = get_importer_path(isolate);
    char *embedded_path = resolve_embedded_relative(registry, module_name, importer);
    if (embedded_path)
        return embedded_path;

    /* Compiler-owned import operands may already carry the graph's typed
     * identity or its physical I/O locator. Route both through the active
     * graph instead of reinterpreting either as a source-language specifier. */
    const XrModuleSpec *resolved_spec = module_spec_for_locator(registry, module_name);
    if (resolved_spec)
        return xr_strdup(resolved_spec->source_path);

    XrModuleResolver *resolver = ensure_resolver(registry);
    if (!resolver)
        return NULL;

    const XrModuleIdentityAuthority *importer_authority =
        module_authority_for_source(registry, importer);
    if (!importer_authority)
        return NULL;

    XrModuleId mid;
    char *err = NULL;
    int rc = xr_module_resolver_resolve(resolver, module_name, importer, importer_authority, &mid,
                                        &err);
    if (err)
        xr_free(err);

    if (rc == 0) {
        char *result = mid.source_path ? xr_strdup(mid.source_path) : NULL;
        xr_module_id_cleanup(&mid);
        return result;
    }

    return NULL;
}

/*
** Detect module type
*/
ModuleType xr_module_detect_type(const char *path) {
    if (!path)
        return MODULE_TYPE_NATIVE;

    // Check extension
    const char *ext = strrchr(path, '.');
    if (ext) {
        if (strcmp(ext, ".xr") == 0) {
            return MODULE_TYPE_SCRIPT;
        }
        if (strcmp(ext, ".so") == 0 || strcmp(ext, ".dylib") == 0 || strcmp(ext, ".dll") == 0) {
            return MODULE_TYPE_NATIVE;
        }
    }

    // Default to native
    return MODULE_TYPE_NATIVE;
}

/* ========== Module Loading ========== */

static bool proto_has_invalid_class_descriptors(XrProto *proto) {
    if (!proto)
        return false;

    uint32_t code_count = (uint32_t) PROTO_CODE_COUNT(proto);
    uint32_t const_count = (uint32_t) PROTO_CONST_COUNT(proto);
    for (uint32_t i = 0; i < code_count; i++) {
        XrInstruction inst = PROTO_CODE(proto, i);
        if (GET_OPCODE(inst) != OP_CLASS_CREATE_FROM_DESCRIPTOR)
            continue;
        uint32_t const_index = (uint32_t) GETARG_Bx(inst);
        if (const_index >= const_count)
            return true;
        XrValue desc_val = PROTO_CONSTANT(proto, const_index);
        if (!XR_IS_PTR(desc_val) || XR_TO_PTR(desc_val) == NULL)
            return true;
    }

    uint32_t sub_count = (uint32_t) PROTO_PROTO_COUNT(proto);
    for (uint32_t i = 0; i < sub_count; i++) {
        if (proto_has_invalid_class_descriptors(PROTO_PROTO(proto, i)))
            return true;
    }
    return false;
}

/*
** Load xray script extension layer
**
** After Native module is loaded, find and execute stdlib/<name>/<name>.xr script extension.
** Exports in the script will be added to the module's export table, can override C module exports.
** The native module is already INITIALIZING; importing it from the extension is a
** circular dependency and fails instead of exposing a partially initialized module.
*/
static void module_script_source_path(const XrModuleRegistry *registry, const char *module_name,
                                      char *path, size_t path_size) {
    snprintf(path, path_size, "%s/%s/%s.xr", registry->stdlib_path, module_name, module_name);
}

static bool load_script_extension(XrVMRuntime *isolate, XrModule *module, const char *module_name) {
    XrModuleRegistry *registry = (XrModuleRegistry *) xr_isolate_get_module_registry(isolate);
    if (!registry) {
        XR_DBG_MODULE("load_script_extension: no registry");
        return true;
    }

    // Set current module context (export will be added to this module)
    XrModule *prev_module = xr_isolate_get_current_module(isolate);
    xr_isolate_set_current_module(isolate, module);

    XrProto *code = NULL;
    char path[XR_PATH_MAX];
    const char *source = NULL;
    char *owned_source = NULL;
    bool embedded_bytecode = false;

#ifndef XR_STDLIB_FROM_FILE
    // Prefer embedded bytecode: bytecode-only embedders can execute pure-Xray
    // stdlib modules without linking the compiler or reading from disk.
    size_t embedded_bc_size = 0;
    const uint8_t *embedded_bc = xr_get_embedded_stdlib_bytecode(module_name, &embedded_bc_size);
    if (embedded_bc && embedded_bc_size > 0) {
        XrBootstrapContainerError bc_error;
        code = xr_bootstrap_container_read(isolate, embedded_bc, embedded_bc_size, &bc_error);
        if (!code) {
            xr_isolate_set_current_module(isolate, prev_module);
            xr_log_warning("module", "failed to load embedded stdlib bytecode for '%s': %d",
                           module_name, bc_error);
            return false;
        }
        if (proto_has_invalid_class_descriptors(code)) {
            xr_instruction_unit_free(code);
            code = NULL;
            XR_DBG_MODULE("load_script_extension: embedded bytecode for %s needs source fallback",
                          module_name);
        } else {
            embedded_bytecode = true;
            XR_DBG_MODULE("load_script_extension: loaded embedded bytecode for %s", module_name);
        }
    }

    // Source fallback keeps the installed/runtime layout independent while the
    // stdlib bytecode generation path is filled in.
    if (!code) {
        source = xr_get_embedded_stdlib(module_name);
        if (source) {
            snprintf(path, sizeof(path), "<embedded stdlib>/%s/%s.xr", module_name, module_name);
            XR_DBG_MODULE("load_script_extension: loaded embedded source for %s", module_name);
        }
    }
#endif

    // Development override from file system (stdlib/<name>/<name>.xr).
    // Pure-Xray stdlib modules (module->requires_script) get all their
    // exports from this layer; hybrid modules may use it to extend or
    // override C exports.
#ifdef XR_STDLIB_FROM_FILE
    if (registry->stdlib_path) {
        module_script_source_path(registry, module_name, path, sizeof(path));

        XR_DBG_MODULE("load_script_extension: trying %s", path);

        owned_source = xr_file_read_all(path, "r", NULL);
        source = owned_source;
        if (owned_source) {
            XR_DBG_MODULE("load_script_extension: loaded from file");
        }
    }
#else
    if (!code && !source && registry->stdlib_path) {
        module_script_source_path(registry, module_name, path, sizeof(path));

        XR_DBG_MODULE("load_script_extension: trying fallback %s", path);

        owned_source = xr_file_read_all(path, "r", NULL);
        source = owned_source;
        if (owned_source) {
            XR_DBG_MODULE("load_script_extension: loaded fallback file");
        }
    }
#endif

    // No extension script: fine for hybrid/pure-C modules, fatal for
    // pure-Xray modules (an empty export table would surface later as
    // confusing "call a non-function value" panics at use sites).
    if (!code && !source) {
        XR_DBG_MODULE("load_script_extension: no extension for '%s'", module_name);
        xr_isolate_set_current_module(isolate, prev_module);
        if (module->requires_script) {
            xr_log_warning("module",
                           "module '%s' requires its script layer but no embedded or file "
                           "artifact was found at '%s'; set XRAY_STDLIB_PATH or reinstall xray",
                           module_name, registry->stdlib_path ? path : "<no stdlib path>");
            return false;
        }
        return true;
    }

    if (!code) {
        if (!registry->compiler_session || !registry->fn_compile_src) {
            xr_isolate_set_current_module(isolate, prev_module);
            xr_free(owned_source);
            xr_log_warning("module",
                           "compiler not available for embedded stdlib source '%s' "
                           "(embedded bytecode missing)",
                           path);
            return false;
        }
        XrModuleIdentityAuthority authority = {
            .kind = XR_MODULE_IDENTITY_STDLIB,
            .namespace_id = module_name,
        };
        code = registry->fn_compile_src(registry->compiler_session, source, path, &authority);
        if (!code) {
            xr_isolate_set_current_module(isolate, prev_module);
            xr_free(owned_source);
            xr_log_warning("module", "failed to compile extension '%s'", path);
            return false;
        }
    }

    // Execute extension script
    XR_DBG_MODULE("before execute: current_module=%s",
                  xr_isolate_get_current_module(isolate)
                      ? xr_isolate_get_current_module(isolate)->name
                      : "null");

    int result = xr_vm_execute_module(isolate, code);

    XR_DBG_MODULE("after execute: result=%d, current_module=%s", result,
                  xr_isolate_get_current_module(isolate)
                      ? xr_isolate_get_current_module(isolate)->name
                      : "null");

    // Cleanup
    xr_free(owned_source);
    xr_isolate_set_current_module(isolate, prev_module);

    if (result != 0) {
        if (embedded_bytecode) {
            XrModuleExecutionFailureIdentity identity =
                xr_module_embedded_execution_failure_identity(code, module_name);
            if (identity.has_source_file) {
                xr_log_warning(
                    "module", "failed to execute embedded stdlib bytecode from '%s' for module '%s'",
                    identity.source_file, identity.module_name);
            } else {
                xr_log_warning("module", "failed to execute embedded stdlib bytecode for module '%s'",
                               identity.module_name);
            }
        } else {
            xr_log_warning("module", "failed to execute extension '%s'", path);
        }
        return false;
    }

    if (!xr_stdlib_vm_fastpath_install(isolate, module, module_name)) {
        xr_log_warning("module", "failed to install generated stdlib-native entries for '%s'",
                       module_name);
        return false;
    }

    return true;
}

/*
** Load Native module (supports hybrid loading)
**
** Flow:
** 1. Create the C module through its registered factory
** 2. Find and execute same-named xray script extension (stdlib/<name>/<name>.xr)
** 3. Script extension can access C module exports and add/override exports
*/
static XrModule *load_native_module(XrVMRuntime *isolate, const char *module_name) {
    XrModuleRegistry *registry = (XrModuleRegistry *) xr_isolate_get_module_registry(isolate);
    if (!registry)
        return NULL;

    XrNativeModuleFactory factory =
        (XrNativeModuleFactory) xr_hashmap_get(registry->native_factories, module_name);
    if (!factory)
        return NULL;

    XrModule *module = factory(isolate);

    if (!module) {
        xr_log_warning("module", "failed to load native module '%s'", module_name);
        return NULL;
    }

    // 2. Add to cache BEFORE loading script extension as an in-progress marker.
    //    Recursive imports observe INITIALIZING and fail with E0504 instead of
    //    observing a partially initialized C layer.
    if (!xr_module_begin_initialization(module)) {
        xr_log_warning("module", "native module '%s' entered an invalid initialization state",
                       module_name);
        xr_module_free(module);
        return NULL;
    }
    if (!xr_hashmap_set(registry->loaded_modules, module_name, module)) {
        xr_log_warning("module", "out of memory caching native module '%s'", module_name);
        xr_module_fail(module);
        xr_module_free(module);
        return NULL;
    }

    // 3. Load xray script extension layer (optional)
    if (!load_script_extension(isolate, module, module_name)) {
        xr_log_warning("module", "failed to load extension for '%s'", module_name);
        xr_hashmap_delete(registry->loaded_modules, module_name);
        xr_module_fail(module);
        xr_module_free(module);
        return NULL;
    }

    if (!xr_module_publish(module)) {
        xr_hashmap_delete(registry->loaded_modules, module_name);
        xr_module_fail(module);
        xr_module_free(module);
        return NULL;
    }

    return module;
}

/*
** Load Script module
**
** Flow:
** 1. Check if file exists
** 2. Read file contents
** 3. Set current module context
** 4. Compile and execute module code
** 5. Return module object (containing exports)
**
** Note: module parameter is an already created module object (for circular dependency detection)
*/
static XrModule *load_script_module(XrVMRuntime *isolate, XrModule *module, const char *path) {
    if (!isolate || !module || !path) {
        return NULL;
    }

    XrModuleRegistry *registry = (XrModuleRegistry *) xr_isolate_get_module_registry(isolate);
    const XrBytecodeModule *embedded = find_embedded_module(registry, path);
    char *source = NULL;
    AstNode *ast = NULL;
    XrProto *code = NULL;

    if (embedded) {
        XrBootstrapContainerError error = XR_BOOTSTRAP_CONTAINER_OK;
        code = xr_bootstrap_container_read(isolate, embedded->bytecode, embedded->bytecode_size, &error);
        if (!code) {
            xr_log_warning("module", "failed to load embedded bytecode '%s': %s", path,
                           xr_bootstrap_container_error_string(error));
            return NULL;
        }
    } else {
        source = xr_file_read_all(path, "r", NULL);
        if (!source)
            return NULL;
    }

    // Set current module context for export collection and relative imports.
    XrModule *prev_module = xr_isolate_get_current_module(isolate);
    xr_isolate_set_current_module(isolate, module);

    // Normalize path, remove redundant "./"
    char *clean_path = normalize_path(path);

    if (!code) {
        if (!registry || !registry->compiler_session || !registry->fn_parse ||
            !registry->fn_compile_ast) {
            xr_isolate_set_current_module(isolate, prev_module);
            xr_free(source);
            xr_free(clean_path);
            xr_log_warning("module", "compiler not available (lite runtime)");
            return NULL;
        }
        ast = registry->fn_parse(registry->compiler_session, source, clean_path);
        if (!ast) {
            xr_isolate_set_current_module(isolate, prev_module);
            xr_free(source);
            xr_free(clean_path);
            return NULL;
        }

        const XrModuleIdentityAuthority *authority =
            module_authority_for_source(registry, clean_path);
        if (!authority) {
            if (registry->fn_ast_free)
                registry->fn_ast_free(ast);
            xr_isolate_set_current_module(isolate, prev_module);
            xr_free(source);
            xr_free(clean_path);
            xr_log_warning("module", "typed module authority unavailable for '%s'", path);
            return NULL;
        }
        code = registry->fn_compile_ast(registry->compiler_session, ast, clean_path, authority);
        if (!code) {
            if (registry->fn_ast_free)
                registry->fn_ast_free(ast);
            xr_isolate_set_current_module(isolate, prev_module);
            xr_free(source);
            xr_free(clean_path);
            return NULL;
        }
    }

    /* The module owns its initializer proto from this point on, including the
     * FAILED state. Memoizing a failed initialization must not orphan the
     * compiled code while preserving the one-shot execution guarantee. */
    module->compiled_code = code;

    // 6. Execute module code (use dedicated module execution function, don't reset VM state)
    void *saved_module_registry = xr_isolate_get_module_registry(isolate);

    int result = xr_vm_execute_module(isolate, code);

    // Restore module_registry that might have been corrupted during VM execution
    if (!xr_isolate_get_module_registry(isolate) && saved_module_registry) {
        xr_isolate_set_module_registry(isolate, saved_module_registry);
    }

    // Execution error should cause module loading to fail
    if (result != 0) {
        if (registry->fn_ast_free)
            registry->fn_ast_free(ast);
        xr_free(source);
        xr_free(clean_path);
        xr_isolate_set_current_module(isolate, prev_module);
        return NULL;
    }

    // 7. Cleanup - code stays module-owned because exported closures reference it.
    if (ast && registry->fn_ast_free)
        registry->fn_ast_free(ast);
    xr_free(source);
    xr_free(clean_path);

    // 8. Restore context
    xr_isolate_set_current_module(isolate, prev_module);

    return module;
}

/* ========== Main Interface Implementation ========== */

/*
** Import module
**
** Import rules:
** - Standard library: import time
** - Third-party package: import alice/utils
** - File import: import "./helper" or import "./models/user"
** - Directory import: import "./models" (via index.xr)
**
** Cache strategy:
** - Native module: use module name as cache key
** - Script module: use resolved absolute path as cache key (ensures module singleton)
*/
XrValue xr_module_import(XrVMRuntime *isolate, const char *module_name) {
    if (!isolate || !module_name) {
        return xr_null();
    }

    XrModuleRegistry *registry = (XrModuleRegistry *) xr_isolate_get_module_registry(isolate);
    if (!registry) {
        return xr_null();
    }

    /* Host-supplied allowlist (used by the MCP runner and other
     * embedders). NULL/empty means every module is permitted, which is
     * the default for CLI / LSP / DAP. When set, an unlisted import is
     * a hard error returned to the VM so untrusted code can never reach
     * dangerous modules. */
    if (!xr_isolate_module_allowed(isolate, module_name)) {
        xr_log_warning("module", "import '%s' rejected: not in isolate allowlist", module_name);
        return xr_null();
    }

    // 1. Check cache (by module name or absolute path)
    XrModule *module = (XrModule *) xr_hashmap_get(registry->loaded_modules, module_name);
    if (module) {
        XrModuleState state = xr_module_state(module);
        if (state == XR_MODULE_INITIALIZING) {
            xr_log_warning("module", "E0504: circular dependency: %s -> %s", module_name,
                           module_name);
            return xr_null();
        }
        return state == XR_MODULE_PUBLISHED ? xr_value_from_module(module) : xr_null();
    }

    // 2. Try to load Native module (standard library C layer)
    // Note: load_native_module already adds to cache internally
    module = load_native_module(isolate, module_name);
    if (module) {
        return xr_value_from_module(module);
    }

    // 3. Try to load Script module (.xr file)
    char *path = xr_module_resolve_path(isolate, module_name);
    if (!path) {
        // Determine if it's a third-party package or local file
        if (strchr(module_name, '/') && module_name[0] != '.' && module_name[0] != '/') {
            // Third-party package format: alice/redis
            fprintf(stderr, "\nError: Package '%s' not found\n\n", module_name);
            fprintf(stderr, "Please install dependency first:\n");
            fprintf(stderr, "  xray pkg add %s\n\n", module_name);
        } else if (module_name[0] == '.' || module_name[0] == '/') {
            // Local file path
            fprintf(stderr, "\nError: Module file '%s' not found\n\n", module_name);
            fprintf(stderr, "Please check:\n");
            fprintf(stderr, "  - File path is correct\n");
            fprintf(stderr, "  - File exists\n\n");
        } else {
            // Might be standard library or unknown module
            fprintf(stderr, "\nError: Module '%s' not found\n\n", module_name);
            fprintf(stderr, "If it's a third-party package, please install first:\n");
            fprintf(stderr, "  xray pkg add <author>/%s\n\n", module_name);
        }
        return xr_null();
    }

    // Normalize path (resolve . and ..), ensure same file uses same cache key
    char *real_path = xr_realpath(path);
    if (real_path) {
        xr_free(path);
        path = real_path;
    }

    // 4. Check cache with absolute path (ensures module singleton)
    module = (XrModule *) xr_hashmap_get(registry->loaded_modules, path);
    if (module) {
        xr_free(path);
        XrModuleState state = xr_module_state(module);
        if (state == XR_MODULE_INITIALIZING) {
            xr_log_warning("module", "E0504: circular dependency: %s", module_name);
            return xr_null();
        }
        return state == XR_MODULE_PUBLISHED ? xr_value_from_module(module) : xr_null();
    }

    // 4. Create and load script module
    module = xr_module_create_script(isolate, module_name, path);
    if (module) {
        if (!xr_module_begin_initialization(module)) {
            xr_module_fail(module);
            xr_free(path);
            return xr_null();
        }
        /* The module-owned absolute path is the stable cache key. The entry is
         * installed before execution so recursion observes INITIALIZING, and
         * retained after execution so later imports observe the terminal
         * PUBLISHED or FAILED state without re-running initialization. */
        if (!xr_hashmap_set(registry->loaded_modules, module->path, module)) {
            xr_log_warning("module", "out of memory caching module '%s'", module_name);
            xr_module_fail(module);
            xr_free(path);
            return xr_null();
        }

        XrModule *loaded = load_script_module(isolate, module, path);

        if (loaded) {
            if (!xr_module_publish(module)) {
                xr_module_fail(module);
                xr_free(path);
                return xr_null();
            }
            xr_free(path);
            return xr_value_from_module(module);
        } else {
            /* FAILED is memoized just like PUBLISHED: module initialization
             * is one-shot, so a later import observes the failed state rather
             * than executing top-level code and cleanups a second time. */
            xr_module_fail(module);
            xr_free(path);
            return xr_null();
        }
    }

    xr_free(path);
    xr_log_warning("module", "module '%s' not found", module_name);
    return xr_null();
}

/*
** Import module member
*/
XrValue xr_module_import_member(XrVMRuntime *isolate, const char *module_name,
                                const char *member_name) {
    if (!isolate || !module_name || !member_name) {
        return xr_null();
    }

    // First import module
    XrValue module_val = xr_module_import(isolate, module_name);
    if (XR_IS_NULL(module_val)) {
        return xr_null();
    }

    XrModule *module = xr_value_to_module(module_val);
    if (!module) {
        return xr_null();
    }

    // Get exported member
    return xr_module_get_export(isolate, module, member_name);
}

/*
** Add current module's export by name (string-based convenience).
** Used by native module registration and legacy paths.
*/
void xr_module_add_current_export(XrVMRuntime *isolate, const char *name, XrValue value,
                                  bool is_const) {
    if (!isolate || !name)
        return;

    XrModule *module = xr_isolate_get_current_module(isolate);
    if (!module) {
        XR_DBG_MODULE("export '%s': no current_module", name);
        return;
    }

    XR_DBG_MODULE("export '%s' to module '%s'", name, module->name);

    XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_register_in_table(sym_table, name);
    xr_module_add_export_sym(isolate, module, sym, value, is_const);
}

/*
** Check if export is a constant (string-based)
*/
bool xr_module_is_export_const(XrVMRuntime *isolate, XrModule *module, const char *name) {
    if (!isolate || !module || !name)
        return false;

    XrSymbolTable *sym_table = (XrSymbolTable *) xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_lookup_in_table(sym_table, name);
    if (sym < 0)
        return false;
    return xr_module_is_const_sym(module, sym);
}

/* ========== Module System Initialization (Standard Library Registration) ========== */

#include "xmodule_factories.h"

typedef struct {
    const char *name;
    XrNativeModuleFactory factory;
} StdlibEntry;

static const StdlibEntry stdlib_core[] = {
    /* prelude is auto-loaded during isolate init by xisolate_full.c so users
     * never need `import prelude`. Listed here so an explicit import is a
     * harmless no-op that resolves through the same registry. */
    {"prelude", xr_native_module_create_prelude},
    {"time", xr_native_module_create_time},
    {"math", xr_native_module_create_math},
    {"path", xr_native_module_create_path},
    {"base64", xr_native_module_create_base64},
    {"regex", xr_native_module_create_regex},
    {"mem", xr_native_module_create_mem},
    {"runtime", xr_native_module_create_runtime},
    {"sync", xr_native_module_create_sync},
    {"parallel", xr_native_module_create_parallel},
    {"simd", xr_native_module_create_simd},
    {"codegen", xr_native_module_create_codegen},
    {"sys", xr_native_module_create_sys},
    {"url", xr_native_module_create_url},
    {"datetime", xr_native_module_create_datetime},
    {"log", xr_native_module_create_log},
    {"encoding", xr_native_module_create_encoding},
    {"text", xr_native_module_create_text},
    /* Pure-Xray stdlib capability probe (task 148 phase 0 item 5): pins the
     * export shapes migrated modules rely on. Tiny and permanent. */
    {"_probe", xr_native_module_create_probe},
};

#if defined(XR_HAS_FILESYSTEM)
static const StdlibEntry stdlib_filesystem[] = {
    {"io", xr_native_module_create_io},
    {"os", xr_native_module_create_os},
};
#endif

#if defined(XR_HAS_TEST_MODULES)
static const StdlibEntry stdlib_test_modules[] = {
    {"test_yield", xr_native_module_create_test_yield},
};
#endif

#if defined(XR_HAS_NETWORK)
static const StdlibEntry stdlib_network[] = {
    {"net", xr_native_module_create_net},
    {"http", xr_native_module_create_http},
};
#endif

#if defined(XR_HAS_WS)
static const StdlibEntry stdlib_ws[] = {
    {"ws", xr_native_module_create_ws},
};
#endif

#if defined(XR_HAS_HTTP2)
static const StdlibEntry stdlib_http2[] = {
    {"http2", xr_native_module_create_http2},
};
#endif

#if defined(XR_HAS_CRYPTO)
static const StdlibEntry stdlib_crypto[] = {
    {"crypto", xr_native_module_create_crypto},
};
#endif

#if defined(XR_HAS_COMPRESS)
static const StdlibEntry stdlib_compress[] = {
    {"compress", xr_native_module_create_compress},
};
#endif

#if defined(XR_HAS_CLUSTER)
static const StdlibEntry stdlib_cluster[] = {
    {"cluster", xr_native_module_create_cluster},
};
#endif

#if defined(XR_HAS_DATA_FORMATS)
static const StdlibEntry stdlib_data_formats[] = {
    {"csv", xr_native_module_create_csv},
    {"toml", xr_native_module_create_toml},
    {"yaml", xr_native_module_create_yaml},
    {"xml", xr_native_module_create_xml},
};
#endif

#define REGISTER_TABLE(table)                                                                      \
    for (int i = 0; i < (int) (sizeof(table) / sizeof(table[0])); i++)                             \
    xr_module_register_native_factory(isolate, table[i].name, table[i].factory)

/*
** Register all standard library modules
** Called after VM initialization
*/
void xr_module_register_stdlib(XrVMRuntime *isolate) {
    if (!isolate)
        return;

    REGISTER_TABLE(stdlib_core);

#if defined(XR_HAS_FILESYSTEM)
    REGISTER_TABLE(stdlib_filesystem);
#endif
#if defined(XR_HAS_TEST_MODULES)
    REGISTER_TABLE(stdlib_test_modules);
#endif
#if defined(XR_HAS_NETWORK)
    REGISTER_TABLE(stdlib_network);
#endif
#if defined(XR_HAS_WS)
    REGISTER_TABLE(stdlib_ws);
#endif
#if defined(XR_HAS_HTTP2)
    REGISTER_TABLE(stdlib_http2);
#endif
#if defined(XR_HAS_CRYPTO)
    REGISTER_TABLE(stdlib_crypto);
#endif
#if defined(XR_HAS_COMPRESS)
    REGISTER_TABLE(stdlib_compress);
#endif
#if defined(XR_HAS_CLUSTER)
    REGISTER_TABLE(stdlib_cluster);
#endif
#if defined(XR_HAS_DATA_FORMATS)
    REGISTER_TABLE(stdlib_data_formats);
#endif
}

#undef REGISTER_TABLE
