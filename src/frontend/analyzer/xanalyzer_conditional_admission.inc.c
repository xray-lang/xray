/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xanalyzer_conditional_admission.inc.c - Reject unchecked declaration requirements
 */
typedef struct XaConditionalAdmission {
    XaAnalyzer *analyzer;
    const char *file;
    unsigned depth;
} XaConditionalAdmission;
static bool xa_check_conditional_admission(AstNode *node, void *pointer) {
    XaConditionalAdmission *check = pointer;
    if (!node) return true;
    const char *message = NULL;
    if (node->type == AST_METHOD_DECL && node->as.method_decl.condition_count)
        message = "conditional method requirements are not supported by this compiler entry";
    else if (check->depth >= 1024 || !xr_ast_node_is_known(node))
        message = "declaration admission cannot inspect this AST";
    if (message) {
        XrLocation location = {0};
        location.file = check->file; location.line = node->line; location.column = node->column;
        xa_analyzer_add_diagnostic(check->analyzer, XR_DIAG_SEV_ERROR, XR_ERR_ANALYZE, message, &location);
        return false;
    }
    ++check->depth;
    bool accepted = xr_ast_for_each_child(node, xa_check_conditional_admission, check);
    --check->depth;
    return accepted;
}
static bool xa_conditional_admission(XaAnalyzer *analyzer, const char *file, AstNode *node) {
    XaConditionalAdmission check = {analyzer, file, 0};
    return xa_check_conditional_admission(node, &check);
}
