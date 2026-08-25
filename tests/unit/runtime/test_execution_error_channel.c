/*
 * test_execution_error_channel.c - Target-neutral error publication contract.
 */

#include "../test_framework.h"
#include "runtime/core/xr_exec_context.h"
#include "runtime/core/xr_runtime_core.h"
#include "runtime/value/xvalue.h"

static XrRuntimeCore core_a;
static XrRuntimeCore core_b;

TEST(publication_uses_exact_active_execution_identity) {
    XrExecutionContext first;
    XrExecutionContext peer;
    XrValue first_pending = xr_null();
    XrValue peer_pending = xr_null();
    xr_exec_context_init(&first, &core_a, NULL);
    xr_exec_context_init(&peer, &core_a, NULL);

    ASSERT(xr_exec_context_bind_error_channel(&first, &first_pending));
    ASSERT(xr_exec_context_bind_error_channel(&peer, &peer_pending));
    XrExecutionContext *previous = xr_exec_context_enter(&peer);

    XrValue error = xr_int(41);
    ASSERT_EQ_INT(xr_exec_context_publish_error_owned(&core_a, &error),
                  XR_EXEC_ERROR_PUBLISH_OK);
    ASSERT(XR_IS_NULL(error));
    ASSERT(XR_IS_NULL(first_pending));
    ASSERT(XR_IS_INT(peer_pending));
    ASSERT_EQ_INT(XR_TO_INT(peer_pending), 41);

    xr_exec_context_restore(previous);
    ASSERT(xr_exec_context_unbind_error_channel(&first, &first_pending));
    ASSERT(xr_exec_context_unbind_error_channel(&peer, &peer_pending));
}

TEST(publication_is_non_overwriting_and_moves_only_on_success) {
    XrExecutionContext context;
    XrValue pending = xr_null();
    xr_exec_context_init(&context, &core_a, NULL);
    ASSERT(xr_exec_context_bind_error_channel(&context, &pending));
    XrExecutionContext *previous = xr_exec_context_enter(&context);

    XrValue first = xr_int(7);
    ASSERT_EQ_INT(xr_exec_context_publish_error_owned(&core_a, &first),
                  XR_EXEC_ERROR_PUBLISH_OK);
    ASSERT(XR_IS_NULL(first));
    ASSERT_EQ_INT(XR_TO_INT(pending), 7);

    XrValue later = xr_int(9);
    ASSERT_EQ_INT(xr_exec_context_publish_error_owned(&core_a, &later),
                  XR_EXEC_ERROR_PUBLISH_CHANNEL_OCCUPIED);
    ASSERT_EQ_INT(XR_TO_INT(later), 9);
    ASSERT_EQ_INT(XR_TO_INT(pending), 7);

    xr_exec_context_restore(previous);
    ASSERT(xr_exec_context_unbind_error_channel(&context, &pending));
}

TEST(publication_rejects_missing_or_wrong_execution_authority) {
    XrExecutionContext context;
    XrValue pending = xr_null();
    XrValue error = xr_int(13);
    xr_exec_context_init(&context, &core_a, NULL);

    xr_exec_context_restore(NULL);
    ASSERT_EQ_INT(xr_exec_context_publish_error_owned(&core_a, &error),
                  XR_EXEC_ERROR_PUBLISH_NO_ACTIVE_CONTEXT);
    ASSERT_EQ_INT(XR_TO_INT(error), 13);
    ASSERT(XR_IS_NULL(pending));

    XrExecutionContext *previous = xr_exec_context_enter(&context);
    ASSERT_EQ_INT(xr_exec_context_publish_error_owned(&core_b, &error),
                  XR_EXEC_ERROR_PUBLISH_WRONG_RUNTIME);
    ASSERT_EQ_INT(XR_TO_INT(error), 13);
    ASSERT_EQ_INT(xr_exec_context_publish_error_owned(&core_a, &error),
                  XR_EXEC_ERROR_PUBLISH_CHANNEL_UNBOUND);
    ASSERT_EQ_INT(XR_TO_INT(error), 13);
    ASSERT(XR_IS_NULL(pending));
    xr_exec_context_restore(previous);
}

TEST(channel_binding_has_exact_borrow_lifetime) {
    XrExecutionContext context;
    XrValue first = xr_null();
    XrValue other = xr_null();
    xr_exec_context_init(&context, &core_a, NULL);

    ASSERT(xr_exec_context_bind_error_channel(&context, &first));
    ASSERT(xr_exec_context_bind_error_channel(&context, &first));
    ASSERT(!xr_exec_context_bind_error_channel(&context, &other));
    ASSERT(!xr_exec_context_unbind_error_channel(&context, &other));

    XrExecutionContext *previous = xr_exec_context_enter(&context);
    ASSERT(!xr_exec_context_bind_error_channel(&context, &first));
    ASSERT(!xr_exec_context_unbind_error_channel(&context, &first));
    xr_exec_context_restore(previous);

    ASSERT(xr_exec_context_unbind_error_channel(&context, &first));
    ASSERT(!xr_exec_context_unbind_error_channel(&context, &first));
    ASSERT(xr_exec_context_bind_error_channel(&context, &other));
    ASSERT(xr_exec_context_unbind_error_channel(&context, &other));
}

TEST(active_teardown_cannot_drop_the_borrowed_channel) {
    XrExecutionContext context;
    XrValue pending = xr_null();
    xr_exec_context_init(&context, &core_a, NULL);
    ASSERT(xr_exec_context_bind_error_channel(&context, &pending));

    XrExecutionContext *previous = xr_exec_context_enter(&context);
    ASSERT(!xr_exec_context_unbind_error_channel(&context, &pending));
    XrValue error = xr_int(23);
    ASSERT_EQ_INT(xr_exec_context_publish_error_owned(&core_a, &error),
                  XR_EXEC_ERROR_PUBLISH_OK);
    ASSERT(XR_IS_NULL(error));
    ASSERT_EQ_INT(XR_TO_INT(pending), 23);

    xr_exec_context_restore(previous);
    ASSERT(xr_exec_context_unbind_error_channel(&context, &pending));
}

TEST(publication_rejects_invalid_owned_source) {
    XrExecutionContext context;
    XrValue pending = xr_null();
    XrValue empty = xr_null();
    xr_exec_context_init(&context, &core_a, NULL);
    ASSERT(xr_exec_context_bind_error_channel(&context, &pending));
    XrExecutionContext *previous = xr_exec_context_enter(&context);

    ASSERT_EQ_INT(xr_exec_context_publish_error_owned(NULL, &empty),
                  XR_EXEC_ERROR_PUBLISH_INVALID_ARGUMENT);
    ASSERT_EQ_INT(xr_exec_context_publish_error_owned(&core_a, NULL),
                  XR_EXEC_ERROR_PUBLISH_INVALID_ARGUMENT);
    ASSERT_EQ_INT(xr_exec_context_publish_error_owned(&core_a, &empty),
                  XR_EXEC_ERROR_PUBLISH_INVALID_ARGUMENT);
    ASSERT(XR_IS_NULL(empty));
    ASSERT(XR_IS_NULL(pending));

    xr_exec_context_restore(previous);
    ASSERT(xr_exec_context_unbind_error_channel(&context, &pending));
}

TEST_MAIN_BEGIN()
    RUN_TEST_SUITE("Execution error channel");
    RUN_TEST(publication_uses_exact_active_execution_identity);
    RUN_TEST(publication_is_non_overwriting_and_moves_only_on_success);
    RUN_TEST(publication_rejects_missing_or_wrong_execution_authority);
    RUN_TEST(channel_binding_has_exact_borrow_lifetime);
    RUN_TEST(active_teardown_cannot_drop_the_borrowed_channel);
    RUN_TEST(publication_rejects_invalid_owned_source);
TEST_MAIN_END()
