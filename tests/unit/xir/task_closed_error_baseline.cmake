set(CLOSED_ERROR_DIR "${CMAKE_CURRENT_BINARY_DIR}/task-closed-error")

set(CLOSED_ERROR_C "${CLOSED_ERROR_DIR}/task_closed_error_native.c")
set(CLOSED_ERROR_HEADER "${CLOSED_ERROR_DIR}/task_closed_error_generated.h")

foreach(family array nested nullable nullable_array tuple enum atomic task_array task reject)
    foreach(backend vm native vm-native native-vm)
        string(REPLACE "-" "_" name "${backend}")

    endforeach()
endforeach()
foreach(family array nested nullable nullable_array tuple enum atomic task_array task)
    foreach(backend vm native vm-native native-vm)
        string(REPLACE "-" "_" name "${backend}")
    endforeach()
endforeach()

foreach(backend vm native vm-native native-vm)
    string(REPLACE "-" "_" name "${backend}")

endforeach()
