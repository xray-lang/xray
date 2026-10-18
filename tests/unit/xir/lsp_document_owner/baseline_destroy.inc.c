/* Byte-exact original destructor body; only its private symbol is renamed. */
static void baseline_document_destroy(XrLspDocument *doc) {
    if (!doc)
        return;
    xlsp_source_syntax_free(doc->syntax_snapshot);
    doc->syntax_snapshot = NULL;
    xlsp_invalidate_import_cache(doc);
    xlsp_free_document_cache(doc);
    xr_arena_destroy(&doc->arena);
    if(doc->source_open)xlsp_source_open_free(doc->source_open);else xr_free(doc->uri);
    if (doc->source_buffer) xlsp_source_buffer_free(doc->source_buffer);
    else { xr_free(doc->content); xr_free(doc->line_offsets); }
    xlsp_source_tokens_free(doc->source_tokens);
    xr_free(doc);
}
