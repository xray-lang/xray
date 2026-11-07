

set(GO_PENDING_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/go_pending_normal")
set(GO_PENDING_TASK_CS)
set(GO_PENDING_VARIANT_CS)
foreach(case body call await)
    list(APPEND GO_PENDING_TASK_CS "${GO_PENDING_DIR}/pending_${case}.c")
endforeach()
foreach(case body nested write)
    list(APPEND GO_PENDING_VARIANT_CS "${GO_PENDING_DIR}/pending_variant_${case}.c")
endforeach()
