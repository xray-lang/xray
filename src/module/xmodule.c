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
#include "xproject.h"
#include "../base/xchecks.h"
#include "../base/xlog.h"
#include "../runtime/xisolate_api.h"
#include "../vm/xvm.h"
#include "../frontend/parser/xast.h"
#include "../frontend/parser/xparse.h"
#include "../base/xmalloc.h"
#include "../runtime/xerror.h"
#include "../runtime/gc/xgc.h"
#include "../base/xhashmap.h"
#include "../runtime/xray_debug.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

/* ========== Forward Declarations ========== */

#include "xbytecode_io.h"

#include "xstdlib_embedded.h"

// xr_vm_execute_module declared in vm/xvm.h (included via xisolate_internal.h → xvm_state.h → ...)

// Compiler hooks — set by xray_isolate_init_common, NULL in lite mode.
// Using function pointers prevents linker from pulling in the entire
// compiler/parser/analyzer chain when only bytecode execution is needed.
static void *(*fn_parse_with_source)(void*, const char*, const char*) = NULL;
static void *(*fn_compile_ast)(void*, void*, const char*) = NULL;
static void *(*fn_compile_source)(void*, const char*, const char*) = NULL;
static void  (*fn_ast_free)(void*) = NULL;

void xr_module_set_compiler_hooks(
    void *(*parse_fn)(void*, const char*, const char*),
    void *(*compile_ast_fn)(void*, void*, const char*),
    void *(*compile_src_fn)(void*, const char*, const char*),
    void  (*ast_free_fn)(void*))
{
    fn_parse_with_source = parse_fn;
    fn_compile_ast = compile_ast_fn;
    fn_compile_source = compile_src_fn;
    fn_ast_free = ast_free_fn;
}

/* ========== Helper Functions ========== */

/*
** Normalize path
** Remove redundant "./" from path, e.g. "a/b/./c" -> "a/b/c"
*/
static char* normalize_path(const char *path) {
    if (!path) return NULL;

    size_t len = strlen(path);
    char *result = (char*)xr_malloc(len + 1);
    char *dst = result;
    const char *src = path;

    while (*src) {
        // Skip "/./" sequences
        if (src[0] == '/' && src[1] == '.' && src[2] == '/') {
            src += 2;  // Skip "/.", keep trailing "/"
            continue;
        }
        // Skip leading "./"
        if (src == path && src[0] == '.' && src[1] == '/') {
            src += 2;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';

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

/*
** Create Native module
*/
XrModule* xr_module_create_native(XrayIsolate *isolate, const char *name) {
    XR_DCHECK(isolate != NULL, "module_create_native: NULL isolate");
    XR_DCHECK(name != NULL, "module_create_native: NULL name");
    XrModule *module = (XrModule*)xr_gc_alloc(xr_isolate_get_gc(isolate), sizeof(XrModule), XR_TMODULE);
    xr_gc_header_init_type(&module->gc, XR_TMODULE);

    module->name = xr_strdup(name);
    module->path = NULL;
    module->module_type = MODULE_TYPE_NATIVE;

    module_init_exports(module);

    module->loaded = false;
    module->loading = false;
    module->native_handle = NULL;
    module->init_fn = NULL;
    module->compiled_code = NULL;

    return module;
}

/*
** Create Script module
*/
XrModule* xr_module_create_script(XrayIsolate *isolate, const char *name, const char *path) {
    XR_DCHECK(isolate != NULL, "module_create_script: NULL isolate");
    XR_DCHECK(name != NULL, "module_create_script: NULL name");
    XrModule *module = (XrModule*)xr_gc_alloc(xr_isolate_get_gc(isolate), sizeof(XrModule), XR_TMODULE);
    xr_gc_header_init_type(&module->gc, XR_TMODULE);

    module->name = xr_strdup(name);
    module->path = xr_strdup(path);
    module->module_type = MODULE_TYPE_SCRIPT;

    module_init_exports(module);

    module->loaded = false;
    module->loading = false;
    module->native_handle = NULL;
    module->init_fn = NULL;
    module->compiled_code = NULL;

    return module;
}

/*
** Add export by SymbolId (core function)
*/
void xr_module_add_export_sym(XrayIsolate *isolate, XrModule *module, SymbolId sym, XrValue value, bool is_const) {
    (void)isolate;
    if (!module) return;

    // Check if symbol already exists (update case)
    for (uint16_t i = 0; i < module->export_count; i++) {
        if (module->export_symbols[i] == sym) {
            module->export_values[i] = value;
            if (is_const) module->export_flags[i] |= XR_EXPORT_CONST;
            return;
        }
    }

    // Grow arrays if needed
    if (module->export_count >= module->export_capacity) {
        uint16_t new_cap = module->export_capacity ? module->export_capacity * 2 : 8;
        XR_REALLOC_OR_ABORT(module->export_values,
                            (size_t)new_cap * sizeof(XrValue),
                            "module export_values grow");
        XR_REALLOC_OR_ABORT(module->export_symbols,
                            (size_t)new_cap * sizeof(SymbolId),
                            "module export_symbols grow");
        XR_REALLOC_OR_ABORT(module->export_flags,
                            (size_t)new_cap * sizeof(uint8_t),
                            "module export_flags grow");
        module->export_capacity = new_cap;
    }

    uint16_t idx = module->export_count++;
    XR_DCHECK(module->export_count <= module->export_capacity, "module_add_export: count > capacity");
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
void xr_module_add_export(XrayIsolate *isolate, XrModule *module, const char *name, XrValue value) {
    if (!isolate || !module || !name) return;

    XrSymbolTable *sym_table = (XrSymbolTable*)xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_register_in_table(sym_table, name);
    xr_module_add_export_sym(isolate, module, sym, value, false);
}

/*
** Get module export (string-based)
*/
XrValue xr_module_get_export(XrayIsolate *isolate, XrModule *module, const char *name) {
    if (!isolate || !module || !name) return xr_null();

    XrSymbolTable *sym_table = (XrSymbolTable*)xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_lookup_in_table(sym_table, name);
    if (sym < 0) return xr_null();
    return xr_module_get_sym(module, sym);
}

/*
** Build sparse SymbolId→index lookup table
** Call after all exports are added (e.g. when module->loaded = true)
*/
void xr_module_build_export_index(XrModule *module) {
    if (!module || module->export_count == 0) return;

    // Free old index
    if (module->symbol_to_index) {
        xr_free(module->symbol_to_index);
        module->symbol_to_index = NULL;
    }

    // Find min/max symbol range
    SymbolId min_sym = module->export_symbols[0];
    SymbolId max_sym = module->export_symbols[0];
    for (uint16_t i = 1; i < module->export_count; i++) {
        if (module->export_symbols[i] < min_sym) min_sym = module->export_symbols[i];
        if (module->export_symbols[i] > max_sym) max_sym = module->export_symbols[i];
    }

    int range = max_sym - min_sym + 1;

    // Safety: if range is absurdly large, skip index (linear scan fallback)
    if (range > 4096) return;

    module->min_symbol = min_sym;
    module->max_symbol = max_sym;
    module->symbol_to_index = (int16_t*)xr_malloc(range * sizeof(int16_t));
    memset(module->symbol_to_index, 0xFF, range * sizeof(int16_t));  // -1 (0xFFFF)

    for (uint16_t i = 0; i < module->export_count; i++) {
        module->symbol_to_index[module->export_symbols[i] - min_sym] = (int16_t)i;
    }
}

/*
** Free module object
*/
void xr_module_free(XrModule *module) {
    if (!module) return;

    if (module->name) xr_free(module->name);
    if (module->path) xr_free(module->path);

    // Free export arrays
    if (module->export_values) xr_free(module->export_values);
    if (module->export_symbols) xr_free(module->export_symbols);
    if (module->export_flags) xr_free(module->export_flags);
    if (module->symbol_to_index) xr_free(module->symbol_to_index);
}

/* ========== Module System Initialization ========== */

/*
** Create module registry
*/
static XrModuleRegistry* create_registry(void) {
    XrModuleRegistry *registry = (XrModuleRegistry*)xr_malloc(sizeof(XrModuleRegistry));
    if (!registry) return NULL;

    registry->native_loaders = xr_hashmap_new();
    registry->loaded_modules = xr_hashmap_new();
    registry->stdlib_path = xr_strdup("stdlib");

    // Project config (optional)
    registry->project = NULL;

    return registry;
}

/*
** Destroy module registry
*/
static void destroy_registry(XrModuleRegistry *registry) {
    if (!registry) return;

    if (registry->native_loaders) {
        xr_hashmap_free(registry->native_loaders);
    }
    if (registry->loaded_modules) {
        // Free compiled_code (XrProto*) for each loaded module before destroying hashmap
        for (uint32_t i = 0; i < registry->loaded_modules->capacity; i++) {
            XrHashMapEntry *entry = &registry->loaded_modules->entries[i];
            if (entry->key != NULL && entry->value != NULL) {
                XrModule *mod = (XrModule*)entry->value;
                if (mod->compiled_code) {
                    xr_free(mod->compiled_code);
                    mod->compiled_code = NULL;
                }
            }
        }
        xr_hashmap_free(registry->loaded_modules);
    }
    if (registry->stdlib_path) {
        xr_free(registry->stdlib_path);
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
void xr_module_system_init(XrayIsolate *isolate) {
    if (!isolate) return;

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
void xr_module_system_init_with_script(XrayIsolate *isolate, const char *script_path) {
    if (!isolate) return;

    // Get or create module registry
    XrModuleRegistry *registry = (XrModuleRegistry*)xr_isolate_get_module_registry(isolate);
    if (!registry) {
        xr_isolate_set_module_registry(isolate, create_registry());
        registry = (XrModuleRegistry*)xr_isolate_get_module_registry(isolate);
        if (!registry) {
            xr_log_warning("module", "failed to create module registry");
            return;
        }
        // Register standard library modules (only on first creation)
        xr_module_register_stdlib(isolate);
    }

    // Try to load project config (for package management)
    if (script_path && !registry->project) {
        // Get script directory
        char *dir = xr_strdup(script_path);
        char *last_slash = strrchr(dir, '/');
        if (last_slash) {
            *last_slash = '\0';
        } else {
            xr_free(dir);
            dir = xr_strdup(".");
        }

        // Try to load project config
        registry->project = xr_project_load(isolate, dir);
        xr_free(dir);
    }
}

/*
** Free module system
*/
void xr_module_system_free(XrayIsolate *isolate) {
    if (!isolate || !xr_isolate_get_module_registry(isolate)) return;

    destroy_registry((XrModuleRegistry*)xr_isolate_get_module_registry(isolate));
    xr_isolate_set_module_registry(isolate, NULL);
}

/* ========== Module Registration ========== */

/*
** Register Native module loader
*/
void xr_module_register_native(XrayIsolate *isolate, const char *name, NativeModuleLoader loader) {
    if (!isolate || !name || !loader) {
        xr_log_warning("module", "invalid arguments to register_native");
        return;
    }

    XrModuleRegistry *registry = (XrModuleRegistry*)xr_isolate_get_module_registry(isolate);
    if (!registry) {
        xr_log_warning("module", "module registry not initialized");
        return;
    }

    // Store loader function pointer
    xr_hashmap_set(registry->native_loaders, name, (void*)loader);
}

/* ========== Path Resolution ========== */

/*
** Resolve module path
**
** Search order:
** 1. Absolute path: use directly
** 2. Relative path (./,../): relative to current script directory
** 3. Third-party package (owner/name): ~/.xray/packages/owner/name/version/
** 4. Current script directory: <script_dir>/<name>.xr
** 5. Standard library directory: stdlib/<name>/<name>.xr
*/
char* xr_module_resolve_path(XrayIsolate *isolate, const char *module_name) {
    if (!module_name || !isolate) return NULL;

    XrModuleRegistry *registry = (XrModuleRegistry*)xr_isolate_get_module_registry(isolate);
    char path[PATH_MAX];

    // 1. Absolute path: return directly
    if (module_name[0] == '/') {
        return xr_strdup(module_name);
    }

    // Get current module directory (prefer path of currently executing module)
    char dir_buf[PATH_MAX];
    const char *script_dir = NULL;

    // Prefer current module's path (supports relative imports within module)
    if (xr_isolate_get_current_module(isolate) && xr_isolate_get_current_module(isolate)->path) {
        strncpy(dir_buf, xr_isolate_get_current_module(isolate)->path, sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        char *last_slash = strrchr(dir_buf, '/');
        if (last_slash) {
            *last_slash = '\0';
            script_dir = dir_buf;
        } else {
            script_dir = ".";
        }
    } else if (xr_isolate_get_script_file(isolate)) {
        // Fall back to main script path
        strncpy(dir_buf, xr_isolate_get_script_file(isolate), sizeof(dir_buf) - 1);
        dir_buf[sizeof(dir_buf) - 1] = '\0';
        char *last_slash = strrchr(dir_buf, '/');
        if (last_slash) {
            *last_slash = '\0';
            script_dir = dir_buf;
        } else {
            script_dir = ".";
        }
    }

    // 2. Relative path: relative to current script directory
    if (strncmp(module_name, "./", 2) == 0 || strncmp(module_name, "../", 3) == 0) {
        if (script_dir) {
            // Handle case with .xr extension
            const char *ext = strrchr(module_name, '.');
            if (ext && strcmp(ext, ".xr") == 0) {
                snprintf(path, sizeof(path), "%s/%s", script_dir, module_name);
                if (access(path, F_OK) == 0) return xr_strdup(path);
            } else {
                // First try path.xr
                snprintf(path, sizeof(path), "%s/%s.xr", script_dir, module_name);
                if (access(path, F_OK) == 0) return xr_strdup(path);

                // Then try path/index.xr (directory entry)
                snprintf(path, sizeof(path), "%s/%s/index.xr", script_dir, module_name);
                if (access(path, F_OK) == 0) return xr_strdup(path);
            }
        }
        return NULL;  // Not found, return NULL
    }

    // 3. Third-party package (owner/name format): ~/.xray/packages/owner/name/version/
    if (strchr(module_name, '/') != NULL) {
        // Get user home directory
        const char *home = getenv("HOME");
        if (home) {
            // Parse owner/name
            char owner[64], name[64];
            if (sscanf(module_name, "%63[^/]/%63s", owner, name) == 2) {
                // Try to find installed package (simplified: use latest version directory)
                // Full implementation needs to read xray.lock or scan version directories
                snprintf(path, sizeof(path), "%s/.xray/packages/%s/%s/latest/src/main.xr",
                         home, owner, name);
                if (access(path, F_OK) == 0) return xr_strdup(path);

                // Try other entry file names
                snprintf(path, sizeof(path), "%s/.xray/packages/%s/%s/latest/main.xr",
                         home, owner, name);
                if (access(path, F_OK) == 0) return xr_strdup(path);
            }
        }
    }

    // 4. Current script directory: <script_dir>/<name>.xr
    if (script_dir) {
        snprintf(path, sizeof(path), "%s/%s.xr", script_dir, module_name);
        if (access(path, F_OK) == 0) return xr_strdup(path);
    }

    // 5. Standard library: stdlib/<name>/<name>.xr
    if (registry && registry->stdlib_path) {
        snprintf(path, sizeof(path), "%s/%s/%s.xr",
                 registry->stdlib_path, module_name, module_name);
        if (access(path, F_OK) == 0) return xr_strdup(path);
    }

    return NULL;
}

/*
** Detect module type
*/
ModuleType xr_module_detect_type(const char *path) {
    if (!path) return MODULE_TYPE_NATIVE;

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

/*
** Load xray script extension layer
**
** After Native module is loaded, find and execute stdlib/<name>/<name>.xr script extension.
** Exports in the script will be added to the module's export table, can override C module exports.
*/
static bool load_script_extension(XrayIsolate *isolate, XrModule *module, const char *module_name) {
    XrModuleRegistry *registry = (XrModuleRegistry*)xr_isolate_get_module_registry(isolate);
    if (!registry) {
        XR_DBG_MODULE("load_script_extension: no registry");
        return true;
    }

    /*
     * Temporarily register module to cache, so import <module_name> in extension script
     * can get the module being loaded (containing C layer exports)
     */
    xr_hashmap_set(registry->loaded_modules, module_name, module);

    // Set current module context (export will be added to this module)
    XrModule *prev_module = xr_isolate_get_current_module(isolate);
    xr_isolate_set_current_module(isolate, module);

    void *code = NULL;
    char path[PATH_MAX];
    char *source = NULL;

    // Load script extension from file system (stdlib/<name>/<name>.xr)
    // This mechanism is preserved for third-party hybrid modules that
    // ship both C and xray script layers. Built-in stdlib modules are
    // now pure C and do not use script extensions.
    if (registry->stdlib_path) {
        snprintf(path, sizeof(path), "%s/%s/%s.xr",
                 registry->stdlib_path, module_name, module_name);

        XR_DBG_MODULE("load_script_extension: trying %s", path);

        FILE *file = fopen(path, "r");
        if (file) {
            fseek(file, 0, SEEK_END);
            long size = ftell(file);
            fseek(file, 0, SEEK_SET);

            source = (char*)xr_malloc(size + 1);
            if (source) {
                size_t read_size = fread(source, 1, size, file);
                source[read_size] = '\0';
            }
            fclose(file);
            XR_DBG_MODULE("load_script_extension: loaded from file");
        }
    }

    // No extension script, return normally
    if (!source) {
        XR_DBG_MODULE("load_script_extension: no extension for '%s'", module_name);
        xr_isolate_set_current_module(isolate, prev_module);
        return true;
    }

    if (!fn_compile_source) {
        xr_isolate_set_current_module(isolate, prev_module);
        xr_free(source);
        xr_log_warning("module", "compiler not available (lite runtime)");
        return false;
    }
    code = fn_compile_source(isolate, source, path);
    if (!code) {
        xr_isolate_set_current_module(isolate, prev_module);
        xr_free(source);
        xr_log_warning("module", "failed to compile extension '%s'", path);
        return false;
    }

    // Execute extension script
    XR_DBG_MODULE("before execute: current_module=%s",
                  xr_isolate_get_current_module(isolate) ? xr_isolate_get_current_module(isolate)->name : "null");

    int result = xr_vm_execute_module(isolate, code);

    XR_DBG_MODULE("after execute: result=%d, current_module=%s",
                  result, xr_isolate_get_current_module(isolate) ? xr_isolate_get_current_module(isolate)->name : "null");

    // Cleanup
    xr_free(source);
    xr_isolate_set_current_module(isolate, prev_module);

    if (result != 0) {
        xr_log_warning("module", "failed to execute extension '%s'", path);
        return false;
    }

    return true;
}

/*
** Load Native module (supports hybrid loading)
**
** Flow:
** 1. Load C module (call registered loader)
** 2. Find and execute same-named xray script extension (stdlib/<name>/<name>.xr)
** 3. Script extension can access C module exports and add/override exports
*/
static XrModule* load_native_module(XrayIsolate *isolate, const char *module_name) {
    XrModuleRegistry *registry = (XrModuleRegistry*)xr_isolate_get_module_registry(isolate);
    if (!registry) return NULL;

    // Find loader from registry
    void *loader_ptr = xr_hashmap_get(registry->native_loaders, module_name);

    if (!loader_ptr) {
        return NULL;  // Not a native module
    }

    // Extract loader function pointer
    NativeModuleLoader loader = (NativeModuleLoader)loader_ptr;

    // 1. Call C loader
    XrModule *module = loader(isolate);

    if (!module) {
        xr_log_warning("module", "failed to load native module '%s'", module_name);
        return NULL;
    }

    // 2. Add to cache BEFORE loading script extension
    //    This prevents circular dependency when extension imports itself (e.g., "import ws as _native")
    module->loading = true;
    xr_hashmap_set(registry->loaded_modules, module_name, module);

    // 3. Load xray script extension layer (optional)
    if (!load_script_extension(isolate, module, module_name)) {
        xr_log_warning("module", "failed to load extension for '%s'", module_name);
        // Extension loading failure doesn't affect C module, continue
    }

    // Build sparse index and mark as loaded
    xr_module_build_export_index(module);
    module->loading = false;
    module->loaded = true;

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
static XrModule* load_script_module(XrayIsolate *isolate, XrModule *module, const char *path) {
    if (!isolate || !module || !path) {
        return NULL;
    }

    const char *module_name = module->name;

    // 1. Check if file exists
    FILE *file = fopen(path, "r");
    if (!file) {
        // File doesn't exist, not an error (might be Native module)
        return NULL;
    }

    // 2. Read file contents
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);

    char *source = (char*)xr_malloc(size + 1);
    if (!source) {
        fclose(file);
        xr_log_warning("module", "memory allocation failed for module '%s'", module_name);
        return NULL;
    }

    size_t read_size = fread(source, 1, size, file);
    source[read_size] = '\0';
    fclose(file);

    // 3. Set current module context (for export collection)
    XrModule *prev_module = xr_isolate_get_current_module(isolate);
    xr_isolate_set_current_module(isolate, module);

    // 5. Parse and compile (API declared in xast.h and xray_isolate_internal.h)

    // Normalize path, remove redundant "./"
    char *clean_path = normalize_path(path);

    if (!fn_parse_with_source || !fn_compile_ast) {
        xr_isolate_set_current_module(isolate, prev_module);
        xr_free(source);
        xr_free(clean_path);
        xr_log_warning("module", "compiler not available (lite runtime)");
        return NULL;
    }
    AstNode *ast = fn_parse_with_source(isolate, source, clean_path);
    if (!ast) {
        xr_isolate_set_current_module(isolate, prev_module);
        xr_free(source);
        xr_free(clean_path);
        return NULL;
    }

    void *code = fn_compile_ast(isolate, ast, clean_path);
    if (!code) {
        if (fn_ast_free) fn_ast_free(ast);
        xr_isolate_set_current_module(isolate, prev_module);
        xr_free(source);
        xr_free(clean_path);
        return NULL;
    }

    // 6. Execute module code (use dedicated module execution function, don't reset VM state)
    void *saved_module_registry = xr_isolate_get_module_registry(isolate);

    int result = xr_vm_execute_module(isolate, code);

    // Restore module_registry that might have been corrupted during VM execution
    if (!xr_isolate_get_module_registry(isolate) && saved_module_registry) {
        xr_isolate_set_module_registry(isolate, saved_module_registry);
    }

    // Execution error should cause module loading to fail
    if (result != 0) {
        if (fn_ast_free) fn_ast_free(ast);
        xr_free(source);
        xr_free(clean_path);
        xr_isolate_set_current_module(isolate, prev_module);
        return NULL;
    }

    // 7. Cleanup - Note: code cannot be freed because exported closures still reference proto
    // Save code to module, free when module is destroyed
    module->compiled_code = code;
    if (fn_ast_free) fn_ast_free(ast);
    xr_free(source);
    xr_free(clean_path);

    // 8. Restore context
    xr_isolate_set_current_module(isolate, prev_module);

    // 9. Build sparse index and mark as loaded
    xr_module_build_export_index(module);
    module->loaded = true;

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
XrValue xr_module_import(XrayIsolate *isolate, const char *module_name) {
    if (!isolate || !module_name) {
        return xr_null();
    }

    XrModuleRegistry *registry = (XrModuleRegistry*)xr_isolate_get_module_registry(isolate);
    if (!registry) {
        return xr_null();
    }

    // 1. Check cache (by module name or absolute path)
    XrModule *module = (XrModule*)xr_hashmap_get(registry->loaded_modules, module_name);
    if (module) {
        // For native modules with script extension: allow "import X as _native" pattern
        // The C layer is already loaded and exports are available
        bool is_native_module = (module->module_type == MODULE_TYPE_NATIVE ||
                                  module->module_type == MODULE_TYPE_MIXED);
        if (module->loading && is_native_module) {
            // Native module loading extension - return C layer exports
            return xr_value_from_module(module);
        }
        if (module->loading) {
            xr_log_warning("module", "circular dependency detected: '%s'", module_name);
            return xr_null();
        }
        return xr_value_from_module(module);
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
    // Note: realpath uses system malloc, must convert to xr_malloc
    char *real_path = realpath(path, NULL);
    if (real_path) {
        xr_free(path);
        path = xr_strdup(real_path);
        free(real_path);
    }

    // 4. Check cache with absolute path (ensures module singleton)
    module = (XrModule*)xr_hashmap_get(registry->loaded_modules, path);
    if (module) {
        xr_free(path);
        if (module->loading) {
            xr_log_warning("module", "circular dependency detected: '%s'", module_name);
            return xr_null();
        }
        return xr_value_from_module(module);
    }

    // 4. Create and load script module
    module = xr_module_create_script(isolate, module_name, path);
    if (module) {
        module->loading = true;
        /*
         * Use absolute path as cache key
         * Note: hashmap doesn't copy key, so path ownership transfers to hashmap
         * Don't free path, it will be freed with hashmap when module system is destroyed
         */
        xr_hashmap_set(registry->loaded_modules, path, module);

        XrModule *loaded = load_script_module(isolate, module, path);

        if (loaded) {
            module->loading = false;
            // path now owned by hashmap, do not free
            return xr_value_from_module(module);
        } else {
            // Loading failed, remove from cache and free path
            xr_hashmap_set(registry->loaded_modules, path, NULL);
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
XrValue xr_module_import_member(XrayIsolate *isolate, const char *module_name, const char *member_name) {
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
** Add current module's export
** Called during OP_EXPORT execution
*/
void xr_module_add_current_export(XrayIsolate *isolate, const char *name, XrValue value, bool is_const) {
    if (!isolate || !name) return;

    XrModule *module = xr_isolate_get_current_module(isolate);
    if (!module) {
        XR_DBG_MODULE("export '%s': no current_module", name);
        return;
    }

    XR_DBG_MODULE("export '%s' to module '%s'", name, module->name);

    XrSymbolTable *sym_table = (XrSymbolTable*)xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_register_in_table(sym_table, name);
    xr_module_add_export_sym(isolate, module, sym, value, is_const);
}

/*
** Check if export is a constant (string-based)
*/
bool xr_module_is_export_const(XrayIsolate *isolate, XrModule *module, const char *name) {
    if (!isolate || !module || !name) return false;

    XrSymbolTable *sym_table = (XrSymbolTable*)xr_isolate_get_symbol_table(isolate);
    SymbolId sym = xr_symbol_lookup_in_table(sym_table, name);
    if (sym < 0) return false;
    return xr_module_is_const_sym(module, sym);
}

/* ========== Module System Initialization (Standard Library Registration) ========== */

#include "xmodule_loaders.h"

typedef struct {
    const char *name;
    NativeModuleLoader loader;
} StdlibEntry;

static const StdlibEntry stdlib_core[] = {
    {"time",     xr_load_module_time},
    {"math",     xr_load_module_math},
    {"json",     xr_load_module_json},
    {"path",     xr_load_module_path},
    {"base64",   xr_load_module_base64},
    {"regex",    xr_load_module_regex},
    {"gc",       xr_load_module_gc},
    {"url",      xr_load_module_url},
    {"datetime", xr_load_module_datetime},
    {"log",      xr_load_module_log},
    {"encoding", xr_load_module_encoding},
};

#if defined(XR_HAS_FILESYSTEM) || !defined(XR_STDLIB_MODULAR)
static const StdlibEntry stdlib_filesystem[] = {
    {"io",         xr_load_module_io},
    {"os",         xr_load_module_os},
    {"test_yield", xr_load_module_test_yield},
};
#endif

#if defined(XR_HAS_NETWORK) || !defined(XR_STDLIB_MODULAR)
static const StdlibEntry stdlib_network[] = {
    {"net",  xr_load_module_net},
    {"http", xr_load_module_http},
    {"ws",   xr_load_module_ws},
};
#endif

#if defined(XR_HAS_CRYPTO) || !defined(XR_STDLIB_MODULAR)
static const StdlibEntry stdlib_crypto[] = {
    {"crypto", xr_load_module_crypto},
};
#endif

#if defined(XR_HAS_COMPRESS) || !defined(XR_STDLIB_MODULAR)
static const StdlibEntry stdlib_compress[] = {
    {"compress", xr_load_module_compress},
};
#endif

#if defined(XR_HAS_CLUSTER) || !defined(XR_STDLIB_MODULAR)
static const StdlibEntry stdlib_cluster[] = {
    {"cluster", xr_load_module_cluster},
};
#endif

#if defined(XR_HAS_DATA_FORMATS) || !defined(XR_STDLIB_MODULAR)
static const StdlibEntry stdlib_data_formats[] = {
    {"csv",  xr_load_module_csv},
    {"toml", xr_load_module_toml},
    {"yaml", xr_load_module_yaml},
    {"xml",  xr_load_module_xml},
};
#endif

#define REGISTER_TABLE(table) \
    for (int i = 0; i < (int)(sizeof(table) / sizeof(table[0])); i++) \
        xr_module_register_native(isolate, table[i].name, table[i].loader)

/*
** Register all standard library modules
** Called after VM initialization
*/
void xr_module_register_stdlib(XrayIsolate *isolate) {
    if (!isolate) return;

    REGISTER_TABLE(stdlib_core);

#if defined(XR_HAS_FILESYSTEM) || !defined(XR_STDLIB_MODULAR)
    REGISTER_TABLE(stdlib_filesystem);
#endif
#if defined(XR_HAS_NETWORK) || !defined(XR_STDLIB_MODULAR)
    REGISTER_TABLE(stdlib_network);
#endif
#if defined(XR_HAS_CRYPTO) || !defined(XR_STDLIB_MODULAR)
    REGISTER_TABLE(stdlib_crypto);
#endif
#if defined(XR_HAS_COMPRESS) || !defined(XR_STDLIB_MODULAR)
    REGISTER_TABLE(stdlib_compress);
#endif
#if defined(XR_HAS_CLUSTER) || !defined(XR_STDLIB_MODULAR)
    REGISTER_TABLE(stdlib_cluster);
#endif
#if defined(XR_HAS_DATA_FORMATS) || !defined(XR_STDLIB_MODULAR)
    REGISTER_TABLE(stdlib_data_formats);
#endif
}

#undef REGISTER_TABLE

