XrJsonValue *xlsp_handle_td_completion_resolve(XrLspServer *server, XrJsonValue *params) {
    XrJsonValue *data = xjson_get_object(params, "data");
    if (!data)
        return xjson_clone(params);

    const char *uri = xjson_get_string(data, "uri");
    const char *name = xjson_get_string(params, "label");

    XrJsonValue *result = xjson_clone(params);
    if (!name)
        return result;

    XaAnalyzer *analyzer = server ? server->workspace_analyzer : NULL;
    char doc_str[XLSP_MAX_PATH];
    int len = 0;
    bool resolved = false;

    // Try analyzer for real type information
    if (analyzer) {
        XaSymbol *sym = xa_analyzer_lookup(analyzer, name);
        if (sym) {
            XaSymbolLinks *links = xa_analyzer_get_links(analyzer, sym);

            if (sym->kind == XA_SYM_FUNCTION || sym->kind == XA_SYM_METHOD) {
                len = snprintf(doc_str, sizeof(doc_str), "```xray\nfn %s(", name);
                if (links && links->param_count > 0) {
                    for (int i = 0; i < links->param_count; i++) {
                        if (i > 0)
                            len += snprintf(doc_str + len, sizeof(doc_str) - len, ", ");
                        const char *pname = (links->param_names && links->param_names[i])
                                                ? links->param_names[i]
                                                : "_";
                        XrType *ptype_obj = (links->param_types && links->param_types[i])
                                                ? links->param_types[i]
                                                : NULL;
                        len = xlsp_append_param_display(doc_str, sizeof(doc_str), len, pname,
                                                        ptype_obj,
                                                        xlsp_function_param_mode(links->type, i));
                    }
                }
                const char *ret =
                    (links && links->return_type) ? xr_type_to_string(links->return_type) : "void";
                snprintf(doc_str + len, sizeof(doc_str) - len, "): %s\n```", ret);
                resolved = true;
            } else if (sym->kind == XA_SYM_CLASS) {
                snprintf(doc_str, sizeof(doc_str), "```xray\nclass %s\n```", name);
                resolved = true;
            } else {
                XrType *type = xa_analyzer_get_type(analyzer, sym);
                const char *type_str = type ? xr_type_to_string(type) : "<error>";
                const char *kw = sym->is_const ? "const" : "var";
                snprintf(doc_str, sizeof(doc_str), "```xray\n%s %s: %s\n```", kw, name, type_str);
                resolved = true;
            }
        }
    }

    // Fallback: generic documentation from symbol table
    if (!resolved && uri) {
        XrLspDocument *doc = xlsp_document_get(server, uri);
        if (doc && doc->ast) {
            snprintf(doc_str, sizeof(doc_str), "Symbol `%s` defined in this module.", name);
            resolved = true;
        }
    }

    if (resolved) {
        XrJsonValue *documentation = xjson_new_object();
        xjson_object_set(documentation, "kind", xjson_new_string("markdown"));
        xjson_object_set(documentation, "value", xjson_new_string(doc_str));
        xjson_object_set(result, "documentation", documentation);
    }

    return result;
}
