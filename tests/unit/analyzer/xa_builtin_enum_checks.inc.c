/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xa_builtin_enum_checks.inc.c - Builtin result declaration identity checks
 */

static bool builtin_enum_source_has_errors(const char *source) {
    XaAnalyzer *analyzer = xa_analyzer_new(g_session);
    if (!analyzer)
        return true;
    AstNode *program = xr_parse(g_session, source);
    if (!program) {
        xa_analyzer_free(analyzer);
        return true;
    }
    xa_analyzer_analyze(analyzer, "builtin-enum-identity.xr", program);
    int count = 0;
    xa_analyzer_get_diagnostics(analyzer, &count);
    bool failed = count != 0;
    xa_analyzer_free(analyzer);
    xr_program_destroy(program);
    setup_pool();
    return failed;
}

TEST(analyzer_builtin_enum_channel_results_keep_declaration_identity) {
    ASSERT(!builtin_enum_source_has_errors(
        "fn sent(result: SendResult) -> bool {\n"
        "  return match (result) { SendResult.Sent -> true, _ -> false }\n"
        "}\n"
        "fn received(result: Recv<i64>) -> i64 {\n"
        "  return match (result) { Recv.Value { value } -> value, _ -> -1 }\n"
        "}\n"
        "const channel: Channel<i64> = Channel(2)\n"
        "var sent_ok = sent(channel.trySend(17))\n"
        "var value = received(channel.tryRecv())\n"
        "var result: Recv<i64> = channel.recv()\n"
        "channel.close()\n"));
}

TEST(analyzer_builtin_enum_task_results_keep_declaration_identity) {
    ASSERT(!builtin_enum_source_has_errors(
        "fn worker() -> i64 { return 17 }\n"
        "fn status(value: TaskStatus) -> TaskStatus { return value }\n"
        "fn result(value: TaskResult<i64>) -> TaskResult<i64> { return value }\n"
        "var task = go worker()\n"
        "var observed = status(task.status)\n"
        "var completed = result(task.awaitResult())\n"));
}

TEST(analyzer_builtin_enum_generic_arguments_remain_distinct) {
    ASSERT(builtin_enum_source_has_errors("fn received(result: Recv<string>) -> () {}\n"
                                          "const channel: Channel<i64> = Channel(1)\n"
                                          "received(channel.tryRecv())\n"));
}

TEST(analyzer_builtin_enum_nested_signatures_bind_exact_heads) {
    XaSymbol *recv = xa_scope_lookup_local(g_analyzer->global_scope, "Recv");
    XaSymbol *send = xa_scope_lookup_local(g_analyzer->global_scope, "SendResult");
    XaSymbol *task = xa_scope_lookup_local(g_analyzer->global_scope, "TaskResult");
    ASSERT(recv && send && task);
    XaSymbolLinks *recv_links = xa_analyzer_get_links(g_analyzer, recv);
    XaSymbolLinks *send_links = xa_analyzer_get_links(g_analyzer, send);
    XaSymbolLinks *task_links = xa_analyzer_get_links(g_analyzer, task);
    ASSERT(recv_links && send_links && task_links);
    XrType *signature = xa_builtin_parse_full_signature(
        g_analyzer, "(handler: (value: Recv<i64>?) -> SendResult): Array<TaskResult<i64>>");
    ASSERT(signature && signature->kind == XR_KIND_FUNCTION);
    ASSERT(signature->function.param_count == 1);
    XrType *handler = signature->function.params[0].type;
    ASSERT(handler && handler->kind == XR_KIND_FUNCTION && handler->function.param_count == 1);
    XrType *parameter = handler->function.params[0].type;
    ASSERT(parameter && parameter->kind == XR_KIND_ENUM && parameter->is_nullable);
    ASSERT(parameter->enum_type.nominal_ref == recv_links->class_info);
    ASSERT(parameter->enum_type.type_arg_count == 1);
    ASSERT(xr_type_equals(parameter->enum_type.type_args[0], xr_type_new_int(NULL)));
    XrType *sent = handler->function.return_type;
    ASSERT(sent && sent->kind == XR_KIND_ENUM);
    ASSERT(sent->enum_type.nominal_ref == send_links->class_info);
    XrType *array = signature->function.return_type;
    ASSERT(array && array->kind == XR_KIND_ARRAY);
    XrType *result = array->container.element_type;
    ASSERT(result && result->kind == XR_KIND_ENUM);
    ASSERT(result->enum_type.nominal_ref == task_links->class_info);
    ASSERT(result->enum_type.type_arg_count == 1);
    ASSERT(XR_TYPE_IS_ERROR(xa_builtin_parse_type_string(g_analyzer, "Recv")));
    ASSERT(XR_TYPE_IS_ERROR(xa_builtin_parse_type_string(g_analyzer, "Recv<i64, bool>")));
}
