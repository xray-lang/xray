/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_source_compile_transaction.c - Original Source and AST entry transaction
 *
 * KEY CONCEPT:
 *   Observe the actual parser's scopes and owned products. Rejected authority
 *   cannot parse or publish; completed and failed parsing restores the same
 *   owner's bindings without reviving retired VM/session statistics.
 */
#include "../test_framework.h"
#include "api/xsource_api.c"
#include "toolchain/xcompiler_session.c"
#include <string.h>

typedef struct CompileTransaction {
    XrSourceCompiler *compiler;
    XrSourceAst *ast;
    XrSourceProgram *programs[3];
    XrSourceDiagnostic *diagnostic;
    XrSourceResult *result;
    char output[32];
    size_t length;
} CompileTransaction;
static CompileTransaction transaction;
static void transaction_close(void) {
    xray_source_ast_free(transaction.ast);
    xray_source_compiler_free(transaction.compiler);
    for(unsigned i=0;i<3;++i)xray_source_program_free(transaction.programs[i]);
    xray_source_diagnostic_free(transaction.diagnostic);
    xray_source_result_free(transaction.result);
    memset(&transaction,0,sizeof(transaction));
}
static XrSourceWriteStatus transaction_output(void *context,XrSourceOutputStream stream,
    const char *bytes,size_t length) {
    CompileTransaction *owner=context;
    if(stream!=XR_SOURCE_STDOUT || length>sizeof(owner->output)-owner->length)
        return XR_SOURCE_WRITE_LIMIT;
    memcpy(owner->output+owner->length,bytes,length);owner->length+=length;
    return XR_SOURCE_WRITE_OK;
}
static void transaction_idle(XrCompilerSession *session) {
    ASSERT_NULL(session->scope);
    ASSERT_NULL(xr_compile_session_current_arena(session));
    ASSERT_NULL(xr_compile_session_string_pool(session));
    ASSERT_EQ_INT(xr_compile_session_resource_status(session),XR_COMPILE_RESOURCE_OK);
}
static void transaction_execute(unsigned index) {
    XrSourceCallOptions runtime;
    ASSERT_EQ_INT(xray_source_call_options_init(&runtime,sizeof(runtime)),XR_SOURCE_OK);
    runtime.write=transaction_output;runtime.context=&transaction;
    ASSERT_EQ_INT(xray_source_execute(transaction.programs[index],&runtime,&transaction.result,NULL),XR_SOURCE_OK);
    int64_t returned=-1;
    ASSERT_EQ_INT(xray_source_result_i64(transaction.result,&returned),XR_SOURCE_OK);
    ASSERT_EQ_INT(returned,0);
    xray_source_result_free(transaction.result);transaction.result=NULL;
    xray_source_program_free(transaction.programs[index]);transaction.programs[index]=NULL;
}
TEST(production_compile_entry_commits_or_aborts_one_operation) {
    XrSourceCompilerOptions options;
    ASSERT_EQ_INT(xray_source_compiler_options_init(&options,sizeof(options)),XR_SOURCE_OK);
    options.stdlib_path=XR_TRANSACTION_STDLIB;
    ASSERT_EQ_INT(xray_source_compiler_new(&options,&transaction.compiler),XR_SOURCE_OK);
    XrCompilerSession *session=xr_source_compiler_session(transaction.compiler);
    ASSERT_NOT_NULL(session);transaction_idle(session);
    XrCompileResources *resources=xr_source_compiler_resources(transaction.compiler);
    ASSERT_TRUE(xr_compile_session_compile_state(session)==xr_source_compiler_state(transaction.compiler));
    ASSERT_TRUE(xr_compile_session_resources(session)==resources);
    uint64_t initial=session->next_scope_token;
    XrSourceText missing={NULL,NULL,"print(42)\n",10};
    ASSERT_EQ_INT(xray_source_compile_text(transaction.compiler,&missing,&transaction.programs[0],NULL),XR_SOURCE_BAD_ARGUMENT);
    ASSERT_NULL(transaction.programs[0]);
    missing.namespace_id="<eval>";
    ASSERT_EQ_INT(xray_source_compile_text(transaction.compiler,&missing,&transaction.programs[0],NULL),XR_SOURCE_BAD_ARGUMENT);
    ASSERT_NULL(transaction.programs[0]);ASSERT_EQ_INT(session->next_scope_token,initial);
    transaction_idle(session);

    const XrSourceText valid={NULL,"compiler-session-fixture-v1","print(42)\n",10};
    ASSERT_EQ_INT(xray_source_parse_text(transaction.compiler,&valid,&transaction.ast,NULL),XR_SOURCE_OK);
    ASSERT_NOT_NULL(transaction.ast);
    ASSERT_EQ_INT(session->next_scope_token,initial+1);
    transaction_idle(session);
    char *identity=transaction.ast->namespace_id;
    transaction.ast->namespace_id=NULL;
    XrSourceStatus absent=xray_source_compile_ast(transaction.compiler,transaction.ast,&transaction.programs[0],NULL);
    transaction.ast->namespace_id=identity;
    ASSERT_EQ_INT(absent,XR_SOURCE_BAD_ARGUMENT);ASSERT_NULL(transaction.programs[0]);
    transaction.ast->namespace_id="<eval>";
    XrSourceStatus invalid=xray_source_compile_ast(transaction.compiler,transaction.ast,&transaction.programs[0],NULL);
    transaction.ast->namespace_id=identity;
    ASSERT_EQ_INT(invalid,XR_SOURCE_BAD_ARGUMENT);ASSERT_NULL(transaction.programs[0]);
    ASSERT_EQ_INT(session->next_scope_token,initial+1);
    transaction_idle(session);

    ASSERT_EQ_INT(xray_source_compile_ast(transaction.compiler,transaction.ast,&transaction.programs[0],NULL),XR_SOURCE_OK);
    ASSERT_NOT_NULL(transaction.programs[0]);
    ASSERT_EQ_INT(session->next_scope_token,initial+1);
    transaction_idle(session);
    uint32_t ast_ids=session->next_ast_id;
    XrCompileResourceStats spent={0},unchanged={0};
    ASSERT_EQ_INT(xr_compile_resources_stats(resources,&spent),XR_COMPILE_RESOURCE_OK);
    XrSourceProgram *occupied=transaction.programs[0];
    ASSERT_EQ_INT(xray_source_compile_ast(transaction.compiler,transaction.ast,&occupied,NULL),XR_SOURCE_BAD_ARGUMENT);
    ASSERT_TRUE(occupied==transaction.programs[0]);
    ASSERT_EQ_INT(xr_compile_resources_stats(resources,&unchanged),XR_COMPILE_RESOURCE_OK);
    ASSERT_EQ_INT(memcmp(&spent,&unchanged,sizeof(spent)),0);
    ASSERT_EQ_INT(session->next_ast_id,ast_ids);
    ASSERT_EQ_INT(xray_source_compile_text(transaction.compiler,&valid,&transaction.programs[1],NULL),XR_SOURCE_OK);
    ASSERT_NOT_NULL(transaction.programs[1]);
    ASSERT_EQ_INT(session->next_scope_token,initial+2);
    transaction_idle(session);

    uint64_t before_failure=session->next_scope_token;
    const XrSourceText broken={NULL,"compiler-session-fixture-v1","fn broken( {\n",13};
    XrSourceStatus rejected=xray_source_compile_text(transaction.compiler,&broken,&transaction.programs[2],&transaction.diagnostic);
    ASSERT_TRUE(rejected!=XR_SOURCE_OK);
    ASSERT_NULL(transaction.programs[2]);ASSERT_NOT_NULL(transaction.diagnostic);
    ASSERT_EQ_INT(session->next_scope_token,before_failure+1);
    transaction_idle(session);
    ASSERT_EQ_INT(xray_source_compile_ast(transaction.compiler,transaction.ast,&transaction.programs[2],NULL),XR_SOURCE_OK);
    ASSERT_NOT_NULL(transaction.programs[2]);
    ASSERT_EQ_INT(session->next_scope_token,before_failure+1);
    transaction_idle(session);
    xray_source_ast_free(transaction.ast);transaction.ast=NULL;
    xray_source_compiler_free(transaction.compiler);transaction.compiler=NULL;
    XrSourceDiagnosticInfo detail={0};
    ASSERT_EQ_INT(xray_source_diagnostic_info(transaction.diagnostic,&detail),XR_SOURCE_OK);
    ASSERT_EQ_INT(detail.status,rejected);ASSERT_TRUE(detail.message && *detail.message);
    printf("\n    owned syntax diagnostic status=%d line=%d column=%d message=%s\n    ",
        detail.status,detail.line,detail.column,detail.message);
    transaction_execute(0);transaction_execute(1);transaction_execute(2);
    ASSERT_EQ_INT(transaction.length,9);
    ASSERT_TRUE(!memcmp(transaction.output,"42\n42\n42\n",9));
}
#undef RUN_TEST
#define RUN_TEST(name) do { \
    unsigned failed=(unsigned)xr_tests_failed; ++xr_tests_run; \
    printf("  %-55s ",#name);fflush(stdout);test_##name();transaction_close(); \
    if((unsigned)xr_tests_failed==failed){++xr_tests_passed;puts("PASS");} \
} while(0)
TEST_MAIN_BEGIN()
RUN_TEST(production_compile_entry_commits_or_aborts_one_operation);
TEST_MAIN_END()
