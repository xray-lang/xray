/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xlsp_imports.c - Cross-file import resolution
 */

#include "xlsp_imports.h"
#include "xlsp_stdlib.h"
#include "../../base/xjson.h"
#include "xlsp_cache.h"
#include "../../base/xhash.h"
#include "../../frontend/lexer/xlex.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include "../../base/xmalloc.h"

#ifndef XR_OS_WINDOWS
#include <libgen.h>
#endif

// Note: exports_cache is now stored in XrLspServer for multi-instance support

// ============================================================================
// Import Parsing
// ============================================================================

// Convert URI to file path
static char *uri_to_path(const char *uri) {
    if (!uri)
        return NULL;

    // Handle file:// prefix
    if (strncmp(uri, "file://", 7) == 0) {
        return xr_strdup(uri + 7);
    }
    return xr_strdup(uri);
}

// Get directory from file path
static char *get_directory(const char *path) {
    if (!path)
        return NULL;

#ifdef XR_OS_WINDOWS
    // Find last separator (/ or \)
    const char *sep = strrchr(path, '/');
    const char *bsep = strrchr(path, '\\');
    if (bsep && (!sep || bsep > sep))
        sep = bsep;
    if (!sep)
        return xr_strdup(".");
    size_t len = (size_t) (sep - path);
    if (len == 0)
        return xr_strdup("/");
    char *result = xr_malloc(len + 1);
    if (!result)
        return NULL;
    memcpy(result, path, len);
    result[len] = '\0';
    return result;
#else
    char *copy = xr_strdup(path);
    char *dir = dirname(copy);
    char *result = xr_strdup(dir);
    xr_free(copy);
    return result;
#endif
}

// Determine import type from path
static XlspImportType get_import_type(const char *path) {
    if (!path)
        return XLSP_IMPORT_STDLIB;

    // Local imports start with "./" or "../"
    if (path[0] == '.') {
        return XLSP_IMPORT_LOCAL;
    }

    // Check if it's a stdlib module
    if (xlsp_stdlib_find_module(path)) {
        return XLSP_IMPORT_STDLIB;
    }

    // Otherwise treat as package (future)
    return XLSP_IMPORT_PACKAGE;
}

// Extract module name from path (e.g. "./utils" -> "utils")
static char *extract_module_name(const char *path) {
    if (!path)
        return NULL;

    // Find last component
    const char *last_slash = strrchr(path, '/');
    const char *name = last_slash ? last_slash + 1 : path;

    // Remove quotes if present
    if (name[0] == '"' || name[0] == '\'')
        name++;

    size_t len = strlen(name);
    if (len > 0 && (name[len - 1] == '"' || name[len - 1] == '\'')) {
        len--;
    }

    char *result = xr_malloc(len + 1);
    memcpy(result, name, len);
    result[len] = '\0';

    return result;
}

XlspImportInfo *xlsp_parse_imports(const char *content, const char *doc_uri) {
    if (!content)
        return NULL;

    XlspImportInfo *head = NULL;
    XlspImportInfo *tail = NULL;

    Scanner scanner;
    xr_scanner_init(&scanner, content);

    Token token;
    while (1) {
        token = xr_scanner_scan(&scanner);
        if (token.type == TK_EOF)
            break;
        if (token.type == TK_ERROR)
            continue;

        // Look for 'import' keyword
        if (token.type == TK_IMPORT) {
            Token next = xr_scanner_scan(&scanner);
            if (next.type == TK_EOF)
                break;

            char *import_path = NULL;

            // import "path" or import identifier
            if (next.type == TK_LITERAL_STRING) {
                // Local/package import: import "./utils"
                import_path = strndup(next.start + 1, next.length - 2);  // Remove quotes
            } else if (next.type == TK_NAME) {
                // Stdlib import: import time
                import_path = strndup(next.start, next.length);
            }

            if (import_path) {
                XlspImportInfo *info = xr_calloc(1, sizeof(XlspImportInfo));
                info->import_path = import_path;
                info->type = get_import_type(import_path);
                info->module_name = extract_module_name(import_path);

                // Resolve local imports
                if (info->type == XLSP_IMPORT_LOCAL && doc_uri) {
                    info->resolved_path = xlsp_resolve_import_path(doc_uri, import_path);
                }

                // Add to list
                if (!head) {
                    head = tail = info;
                } else {
                    tail->next = info;
                    tail = info;
                }
            }
        }
    }

    return head;
}

void xlsp_free_imports(XlspImportInfo *imports) {
    while (imports) {
        XlspImportInfo *next = imports->next;
        xr_free(imports->module_name);
        xr_free(imports->import_path);
        xr_free(imports->resolved_path);
        xr_free(imports);
        imports = next;
    }
}

// ============================================================================
// Path Resolution
// ============================================================================

char *xlsp_resolve_import_path(const char *base_uri, const char *import_path) {
    if (!base_uri || !import_path)
        return NULL;

    // Only resolve local imports
    if (import_path[0] != '.')
        return NULL;

    char *base_path = uri_to_path(base_uri);
    if (!base_path)
        return NULL;

    char *base_dir = get_directory(base_path);
    xr_free(base_path);

    if (!base_dir)
        return NULL;

    // Build full path
    size_t path_len = strlen(base_dir) + strlen(import_path) + 8;  // +8 for ".xr" and slashes
    char *full_path = xr_malloc(path_len);

    snprintf(full_path, path_len, "%s/%s.xr", base_dir, import_path);
    xr_free(base_dir);

    // Normalize path (handle ../ etc)
#ifdef XR_OS_WINDOWS
    char resolved_buf[_MAX_PATH];
    char *resolved = _fullpath(resolved_buf, full_path, _MAX_PATH);
    xr_free(full_path);
    return resolved ? xr_strdup(resolved) : NULL;
#else
    char *resolved = realpath(full_path, NULL);
    xr_free(full_path);
    return resolved;
#endif
}

// ============================================================================
// Export Extraction
// ============================================================================

void xlsp_free_exports(XlspExportedSymbol *symbols) {
    while (symbols) {
        XlspExportedSymbol *next = symbols->next;
        xr_free(symbols->name);
        xr_free(symbols->signature);
        xr_free(symbols->documentation);
        xr_free(symbols);
        symbols = next;
    }
}

static void free_file_exports(XlspFileExports *exports) {
    if (!exports)
        return;
    xr_free(exports->file_path);
    xlsp_free_exports(exports->symbols);
    xr_free(exports);
}

// Hash function for exports cache bucket lookup
static uint32_t exports_cache_hash(const char *path) {
    return xr_hash_bytes(path, strlen(path)) % EXPORTS_CACHE_BUCKETS;
}

// Ensure exports cache is allocated
static XlspExportsCache *ensure_exports_cache(XrLspServer *server) {
    if (!server->exports_cache) {
        server->exports_cache = xr_calloc(1, sizeof(XlspExportsCache));
    }
    return server->exports_cache;
}

// Get cached exports or NULL
static XlspFileExports *get_cached_exports(XrLspServer *server, const char *file_path) {
    if (!server || !server->exports_cache)
        return NULL;
    struct stat st;
    if (stat(file_path, &st) != 0)
        return NULL;

    uint32_t h = exports_cache_hash(file_path);
    for (XlspFileExports *e = server->exports_cache->buckets[h]; e; e = e->next) {
        if (strcmp(e->file_path, file_path) == 0) {
            if (e->mtime == st.st_mtime) {
                return e;
            }
            break;
        }
    }
    return NULL;
}

// Add to cache
static void cache_exports(XrLspServer *server, const char *file_path, XlspExportedSymbol *symbols) {
    if (!server)
        return;
    struct stat st;
    if (stat(file_path, &st) != 0)
        return;

    XlspExportsCache *cache = ensure_exports_cache(server);
    if (!cache)
        return;

    uint32_t h = exports_cache_hash(file_path);

    // Remove old entry if exists
    XlspFileExports **pp = &cache->buckets[h];
    while (*pp) {
        if (strcmp((*pp)->file_path, file_path) == 0) {
            XlspFileExports *old = *pp;
            *pp = old->next;
            free_file_exports(old);
            break;
        }
        pp = &(*pp)->next;
    }

    // Add new entry at bucket head
    XlspFileExports *entry = xr_calloc(1, sizeof(XlspFileExports));
    entry->file_path = xr_strdup(file_path);
    entry->symbols = symbols;
    entry->mtime = st.st_mtime;
    entry->next = cache->buckets[h];
    cache->buckets[h] = entry;
}

XlspExportedSymbol *xlsp_extract_exports(XrLspServer *server, const char *file_path) {
    if (!file_path)
        return NULL;

    // Check cache first
    XlspFileExports *cached = get_cached_exports(server, file_path);
    if (cached) {
        // Return a copy of cached symbols
        XlspExportedSymbol *head = NULL;
        XlspExportedSymbol *tail = NULL;
        for (XlspExportedSymbol *s = cached->symbols; s; s = s->next) {
            XlspExportedSymbol *copy = xr_calloc(1, sizeof(XlspExportedSymbol));
            copy->name = xr_strdup(s->name);
            copy->kind = s->kind;
            if (s->signature)
                copy->signature = xr_strdup(s->signature);
            if (s->documentation)
                copy->documentation = xr_strdup(s->documentation);
            copy->line = s->line;
            if (!head)
                head = tail = copy;
            else {
                tail->next = copy;
                tail = copy;
            }
        }
        return head;
    }

    // Read file
    FILE *f = fopen(file_path, "r");
    if (!f)
        return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *content = xr_malloc(size + 1);
    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';
    fclose(f);

    // Parse exports
    XlspExportedSymbol *head = NULL;
    XlspExportedSymbol *tail = NULL;

    Scanner scanner;
    xr_scanner_init(&scanner, content);

    Token token;
    while (1) {
        token = xr_scanner_scan(&scanner);
        if (token.type == TK_EOF)
            break;
        if (token.type == TK_ERROR)
            continue;

        // Look for 'export' keyword
        if (token.type == TK_EXPORT) {
            Token next = xr_scanner_scan(&scanner);
            if (next.type == TK_EOF)
                break;

            XlspExportedSymbol *sym = NULL;

            // export fn name
            if (next.type == TK_FN) {
                Token name_tok = xr_scanner_scan(&scanner);
                if (name_tok.type == TK_NAME) {
                    sym = xr_calloc(1, sizeof(XlspExportedSymbol));
                    sym->name = strndup(name_tok.start, name_tok.length);
                    sym->kind = 12;  // Function
                    sym->line = name_tok.line;

                    // Try to extract signature (simplified)
                    Token paren = xr_scanner_scan(&scanner);
                    if (paren.type == TK_LPAREN) {
                        // Build signature
                        char sig_buf[256];
                        int sig_len = snprintf(sig_buf, sizeof(sig_buf), "fn %s(", sym->name);
                        int depth = 1;
                        bool first_param = true;

                        while (depth > 0) {
                            Token t = xr_scanner_scan(&scanner);
                            if (t.type == TK_EOF)
                                break;
                            if (t.type == TK_LPAREN)
                                depth++;
                            else if (t.type == TK_RPAREN)
                                depth--;

                            if (depth > 0 && t.type == TK_NAME) {
                                if (!first_param && sig_len < (int) sizeof(sig_buf) - 10) {
                                    sig_len += snprintf(sig_buf + sig_len,
                                                        sizeof(sig_buf) - sig_len, ", ");
                                }
                                first_param = false;
                                int param_len = t.length < (int) (sizeof(sig_buf) - sig_len - 5)
                                                    ? t.length
                                                    : (int) (sizeof(sig_buf) - sig_len - 5);
                                memcpy(sig_buf + sig_len, t.start, param_len);
                                sig_len += param_len;
                                sig_buf[sig_len] = '\0';
                            }
                        }
                        if (sig_len < (int) sizeof(sig_buf) - 2) {
                            sig_buf[sig_len++] = ')';
                            sig_buf[sig_len] = '\0';
                        }
                        sym->signature = xr_strdup(sig_buf);
                    }
                }
            }
            // export const name
            else if (next.type == TK_CONST) {
                Token name_tok = xr_scanner_scan(&scanner);
                if (name_tok.type == TK_NAME) {
                    sym = xr_calloc(1, sizeof(XlspExportedSymbol));
                    sym->name = strndup(name_tok.start, name_tok.length);
                    sym->kind = 14;  // Constant
                    sym->line = name_tok.line;
                }
            }
            // export class name
            else if (next.type == TK_CLASS) {
                Token name_tok = xr_scanner_scan(&scanner);
                if (name_tok.type == TK_NAME) {
                    sym = xr_calloc(1, sizeof(XlspExportedSymbol));
                    sym->name = strndup(name_tok.start, name_tok.length);
                    sym->kind = 5;  // Class
                    sym->line = name_tok.line;
                }
            }

            if (sym) {
                if (!head)
                    head = tail = sym;
                else {
                    tail->next = sym;
                    tail = sym;
                }
            }
        }
    }

    xr_free(content);

    // Cache the result (make a copy for cache)
    XlspExportedSymbol *cache_head = NULL;
    XlspExportedSymbol *cache_tail = NULL;
    for (XlspExportedSymbol *s = head; s; s = s->next) {
        XlspExportedSymbol *copy = xr_calloc(1, sizeof(XlspExportedSymbol));
        copy->name = xr_strdup(s->name);
        copy->kind = s->kind;
        if (s->signature)
            copy->signature = xr_strdup(s->signature);
        if (s->documentation)
            copy->documentation = xr_strdup(s->documentation);
        copy->line = s->line;
        if (!cache_head)
            cache_head = cache_tail = copy;
        else {
            cache_tail->next = copy;
            cache_tail = copy;
        }
    }
    cache_exports(server, file_path, cache_head);

    return head;
}

// ============================================================================
// Import Caching
// ============================================================================

// Get cached imports for a document, parsing if necessary
// Returns pointer to cached list (do NOT free - owned by document)
static XlspImportInfo *get_document_imports(XrLspDocument *doc) {
    if (!doc || !doc->content)
        return NULL;

    // Check if cache is valid (content hash matches)
    if (doc->cached_imports && doc->imports_content_hash == doc->content_hash) {
        return doc->cached_imports;
    }

    // Cache invalid - free old and re-parse
    if (doc->cached_imports) {
        xlsp_free_imports(doc->cached_imports);
        doc->cached_imports = NULL;
    }

    // Parse and cache
    doc->cached_imports = xlsp_parse_imports(doc->content, doc->uri);
    doc->imports_content_hash = doc->content_hash;

    return doc->cached_imports;
}

// Invalidate import cache (call when document content changes)
void xlsp_invalidate_import_cache(XrLspDocument *doc) {
    if (!doc)
        return;
    if (doc->cached_imports) {
        xlsp_free_imports(doc->cached_imports);
        doc->cached_imports = NULL;
    }
    doc->imports_content_hash = XLSP_CONTENT_HASH_UNINITIALIZED;
}

// ============================================================================
// Integration APIs
// ============================================================================

// Find import info by module name (uses cache)
static XlspImportInfo *find_import(XrLspDocument *doc, const char *module_name) {
    if (!doc || !doc->content || !module_name)
        return NULL;

    // Use cached imports
    XlspImportInfo *imports = get_document_imports(doc);

    for (XlspImportInfo *imp = imports; imp; imp = imp->next) {
        if (strcmp(imp->module_name, module_name) == 0) {
            // Found - return a copy (caller owns it)
            XlspImportInfo *result = xr_calloc(1, sizeof(XlspImportInfo));
            if (!result)
                return NULL;

            result->module_name = xr_strdup(imp->module_name);
            result->import_path = imp->import_path ? xr_strdup(imp->import_path) : NULL;
            result->resolved_path = imp->resolved_path ? xr_strdup(imp->resolved_path) : NULL;
            result->type = imp->type;
            return result;
        }
    }

    return NULL;
}

XrJsonValue *xlsp_get_import_completions(XrLspDocument *doc, const char *module_name) {
    XrJsonValue *items = xjson_new_array();

    XlspImportInfo *imp = find_import(doc, module_name);
    if (!imp)
        return items;

    if (imp->type == XLSP_IMPORT_STDLIB) {
        // Use stdlib completions
        const XlspModuleInfo *module = xlsp_stdlib_find_module(module_name);
        if (module) {
            for (int i = 0; i < module->symbol_count; i++) {
                const XlspSymbolInfo *sym = &module->symbols[i];
                XrJsonValue *item = xjson_new_object();
                xjson_object_set(item, "label", xjson_new_string(sym->name));
                xjson_object_set(item, "kind", xjson_new_number(sym->kind));
                if (sym->signature) {
                    xjson_object_set(item, "detail", xjson_new_string(sym->signature));
                }
                if (sym->documentation) {
                    xjson_object_set(item, "documentation", xjson_new_string(sym->documentation));
                }
                xjson_array_push(items, item);
            }
        }
    } else if (imp->type == XLSP_IMPORT_LOCAL && imp->resolved_path) {
        // Use local file exports
        XlspExportedSymbol *exports = xlsp_extract_exports(doc->server, imp->resolved_path);
        for (XlspExportedSymbol *sym = exports; sym; sym = sym->next) {
            XrJsonValue *item = xjson_new_object();
            xjson_object_set(item, "label", xjson_new_string(sym->name));
            xjson_object_set(item, "kind", xjson_new_number(sym->kind));
            if (sym->signature) {
                xjson_object_set(item, "detail", xjson_new_string(sym->signature));
            }
            xjson_array_push(items, item);
        }
        xlsp_free_exports(exports);
    }

    xlsp_free_imports(imp);
    return items;
}

const char *xlsp_get_import_hover(XrLspDocument *doc, const char *module_name,
                                  const char *symbol_name, char *buf, size_t buf_size) {
    XlspImportInfo *imp = find_import(doc, module_name);
    if (!imp)
        return NULL;

    const char *result = NULL;

    if (imp->type == XLSP_IMPORT_STDLIB) {
        const XlspModuleInfo *module = xlsp_stdlib_find_module(module_name);
        if (module) {
            const XlspSymbolInfo *sym = xlsp_stdlib_find_symbol(module, symbol_name);
            if (sym) {
                snprintf(buf, buf_size, "```xray\n%s.%s\n%s\n```\n\n%s", module_name, sym->name,
                         sym->signature ? sym->signature : "",
                         sym->documentation ? sym->documentation : "");
                result = buf;
            }
        }
    } else if (imp->type == XLSP_IMPORT_LOCAL && imp->resolved_path) {
        XlspExportedSymbol *exports = xlsp_extract_exports(doc->server, imp->resolved_path);
        for (XlspExportedSymbol *sym = exports; sym; sym = sym->next) {
            if (strcmp(sym->name, symbol_name) == 0) {
                snprintf(buf, buf_size, "```xray\n%s.%s\n%s\n```\n\n(from %s)", module_name,
                         sym->name, sym->signature ? sym->signature : "", imp->import_path);
                result = buf;
                break;
            }
        }
        xlsp_free_exports(exports);
    }

    xlsp_free_imports(imp);
    return result;
}

XrJsonValue *xlsp_get_import_definition(XrLspDocument *doc, const char *module_name,
                                        const char *symbol_name) {
    XlspImportInfo *imp = find_import(doc, module_name);
    if (!imp)
        return NULL;

    XrJsonValue *result = NULL;

    if (imp->type == XLSP_IMPORT_LOCAL && imp->resolved_path) {
        XlspExportedSymbol *exports = xlsp_extract_exports(doc->server, imp->resolved_path);
        for (XlspExportedSymbol *sym = exports; sym; sym = sym->next) {
            if (strcmp(sym->name, symbol_name) == 0) {
                result = xjson_new_object();

                // Build file URI
                char uri[512];
                snprintf(uri, sizeof(uri), "file://%s", imp->resolved_path);
                xjson_object_set(result, "uri", xjson_new_string(uri));

                // Range at symbol line
                XrJsonValue *range = xjson_new_object();
                XrJsonValue *start = xjson_new_object();
                xjson_object_set(start, "line", xjson_new_number(sym->line - 1));
                xjson_object_set(start, "character", xjson_new_number(0));
                XrJsonValue *end = xjson_new_object();
                xjson_object_set(end, "line", xjson_new_number(sym->line - 1));
                xjson_object_set(end, "character", xjson_new_number(100));
                xjson_object_set(range, "start", start);
                xjson_object_set(range, "end", end);
                xjson_object_set(result, "range", range);
                break;
            }
        }
        xlsp_free_exports(exports);
    }

    xlsp_free_imports(imp);
    return result;
}

XrJsonValue *xlsp_get_module_file_location(XrLspDocument *doc, const char *module_name) {
    if (!doc || !module_name)
        return NULL;

    // Use cached imports
    XlspImportInfo *imports = get_document_imports(doc);
    XrJsonValue *result = NULL;

    for (XlspImportInfo *imp = imports; imp; imp = imp->next) {
        if (strcmp(imp->module_name, module_name) == 0) {
            if (imp->type == XLSP_IMPORT_LOCAL && imp->resolved_path) {
                result = xjson_new_object();

                // Build file URI
                char uri[512];
                snprintf(uri, sizeof(uri), "file://%s", imp->resolved_path);
                xjson_object_set(result, "uri", xjson_new_string(uri));

                // Range at file start
                XrJsonValue *range = xjson_new_object();
                XrJsonValue *start = xjson_new_object();
                xjson_object_set(start, "line", xjson_new_number(0));
                xjson_object_set(start, "character", xjson_new_number(0));
                XrJsonValue *end = xjson_new_object();
                xjson_object_set(end, "line", xjson_new_number(0));
                xjson_object_set(end, "character", xjson_new_number(0));
                xjson_object_set(range, "start", start);
                xjson_object_set(range, "end", end);
                xjson_object_set(result, "range", range);
            }
            break;
        }
    }

    // Note: do NOT free imports - they are owned by document cache
    return result;
}

void xlsp_exports_cache_remove(XrLspServer *server, const char *file_path) {
    if (!server || !server->exports_cache || !file_path)
        return;

    uint32_t h = exports_cache_hash(file_path);
    XlspFileExports **pp = &server->exports_cache->buckets[h];
    while (*pp) {
        if (strcmp((*pp)->file_path, file_path) == 0) {
            XlspFileExports *old = *pp;
            *pp = old->next;
            free_file_exports(old);
            return;
        }
        pp = &(*pp)->next;
    }
}

void xlsp_exports_cache_remove_prefix(XrLspServer *server, const char *prefix) {
    if (!server || !server->exports_cache || !prefix)
        return;

    size_t prefix_len = strlen(prefix);
    for (int i = 0; i < EXPORTS_CACHE_BUCKETS; i++) {
        XlspFileExports **pp = &server->exports_cache->buckets[i];
        while (*pp) {
            if ((*pp)->file_path && strncmp((*pp)->file_path, prefix, prefix_len) == 0) {
                XlspFileExports *old = *pp;
                *pp = old->next;
                free_file_exports(old);
            } else {
                pp = &(*pp)->next;
            }
        }
    }
}

void xlsp_free_exports_cache(XrLspServer *server) {
    if (!server || !server->exports_cache)
        return;

    // Free all cached exports across all buckets
    for (int i = 0; i < EXPORTS_CACHE_BUCKETS; i++) {
        XlspFileExports *e = server->exports_cache->buckets[i];
        while (e) {
            XlspFileExports *next = e->next;
            xr_free(e->file_path);
            xlsp_free_exports(e->symbols);
            xr_free(e);
            e = next;
        }
    }
    xr_free(server->exports_cache);
    server->exports_cache = NULL;
}
