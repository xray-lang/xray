/* C11 generated producers transfer real Value30 owners to a C++17 client.
 * The audit owner retains only the allocation ledger, never code or execution. */
#ifndef TEST_TASK_RESULT_BRIDGE_H
#define TEST_TASK_RESULT_BRIDGE_H
#include "xir/xxir_value.h"
typedef struct XrXirTypes XrXirTypes;
typedef struct TestTaskResultOwner TestTaskResultOwner;
#if defined(__cplusplus)
extern "C" {
#endif
void test_task_result_produce(const char *const names[4],const char *expected,
    XrXirValue values[4],const XrXirTypes **types,TestTaskResultOwner **owner);
void test_task_result_observe(const XrXirValue values[4]);
void test_task_result_finish(TestTaskResultOwner *owner);
#if defined(__cplusplus)
}
#endif
#endif
