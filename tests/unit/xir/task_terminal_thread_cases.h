/* A retained public task crosses threads before and after driver publication. */
typedef struct TaskThreadCase {
    const XrXirValue *handle;
    _Atomic(uint32_t) *ready;
    uint32_t terminal_copies;
} TaskThreadCase;
static void task_thread_copies(TaskThreadCase *worker) {
    XrXirValue copy = {0}; XrXirCallResult outcome = {0};
    CHECK(xr_xir_value_copy(worker->handle, &copy) == XR_XIR_VALUE_OK);
    CHECK(xr_xir_task_copy_outcome(&copy, &outcome) == XR_XIR_CALL_BAD_STATE);
    xr_xir_value_drop(&copy);
    atomic_fetch_add_explicit(worker->ready, 1, memory_order_release);
    uint32_t pending = 0;
    for (;;) {
        CHECK(xr_xir_value_copy(worker->handle, &copy) == XR_XIR_VALUE_OK);
        XrXirCallStatus status = xr_xir_task_copy_outcome(&copy, &outcome);
        xr_xir_value_drop(&copy);
        if (status == XR_XIR_CALL_BAD_STATE) { CHECK(++pending < 1000000); continue; }
        CHECK(status == XR_XIR_CALL_RETURNED);
        task_assert_bytes(&outcome.value); xr_xir_call_result_drop(&outcome);
        if (++worker->terminal_copies == 10000) break;
    }
}
#if defined(XR_OS_WINDOWS)
static DWORD WINAPI task_thread_entry(void *pointer) { task_thread_copies(pointer); return 0; }
#else
static void *task_thread_entry(void *pointer) { task_thread_copies(pointer); return NULL; }
#endif
static void task_terminal_threads(void) {
    TaskFixture fixture = {0}; task_fixture_new(&fixture);
    XrXirDomain *domain = NULL; CHECK(xr_xir_domain_new(1048576, &domain) == XR_XIR_VALUE_OK);
    XrXirTaskExecutorConfig config = task_config(&fixture, domain);
    XrXirTaskExecutor *executor = NULL;
    CHECK(task_test_executor_new(&config, &executor) == XR_XIR_CALL_READY);
    XrXirValue handle = {0}; XrXirCallRequest request = {4, NULL, 0};
    CHECK(xr_xir_task_executor_spawn(executor, (XrXirType)257, &request, &handle) == XR_XIR_CALL_READY);
    _Atomic(uint32_t) ready = 0;
    TaskThreadCase cases[] = {{&handle, &ready, 0}, {&handle, &ready, 0}};
#if defined(XR_OS_WINDOWS)
    HANDLE threads[2];
#else
    pthread_t threads[2];
#endif
    for (uint32_t i = 0; i < 2; ++i) {
#if defined(XR_OS_WINDOWS)
        threads[i] = CreateThread(NULL, 0, task_thread_entry, &cases[i], 0, NULL); CHECK(threads[i]);
#else
        CHECK(pthread_create(&threads[i], NULL, task_thread_entry, &cases[i]) == 0);
#endif
    }
    uint32_t spins = 0;
    while (atomic_load_explicit(&ready, memory_order_acquire) != 2) CHECK(++spins < 100000000);
    while (xr_xir_task_executor_poll(executor, 1) == XR_XIR_CALL_READY) { }
    for (uint32_t i = 0; i < 2; ++i) {
#if defined(XR_OS_WINDOWS)
        CHECK(WaitForSingleObject(threads[i], INFINITE) == WAIT_OBJECT_0); CHECK(CloseHandle(threads[i]));
#else
        CHECK(pthread_join(threads[i], NULL) == 0);
#endif
        CHECK(cases[i].terminal_copies == 10000);
    }
    task_fixture_drop(&fixture); CHECK(xr_xir_task_executor_free(executor, NULL) == XR_XIR_CALL_READY);
    XrXirDomainBudgetStats costs = xr_xir_domain_budget_stats(domain);
    CHECK(costs.bound && costs.work < config.work_limit && !costs.metadata_live && !costs.call_live);
    xr_xir_domain_drop(domain); xr_xir_value_drop(&handle);
    CHECK(fixture.witness.code_releases == 1 && !runtime_live && !runtime_bytes);
    puts("Task pending and release-published terminal handle copied across two threads, 20000 sticky string observations and physical0");
}
