# The compiler-owner consumers use the same runtime foundation as production.
# Their observing compiler ledger remains independent of runtime allocations.
target_sources(xir_compile_owner_runtime PRIVATE
    "${PROJECT_SOURCE_DIR}/src/xir/xxir_scalar_core.c"
    "${PROJECT_SOURCE_DIR}/src/xir/xxir_task.c"
    "${PROJECT_SOURCE_DIR}/src/xir/xxir_task_budget.c")
