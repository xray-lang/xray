

set(TASK_SCALAR_DIR "${CMAKE_CURRENT_BINARY_DIR}/task-scalar-results")
set(TASK_SCALAR_C "${TASK_SCALAR_DIR}/task_scalar_native.c")
set(TASK_SCALAR_HEADER "${TASK_SCALAR_DIR}/task_scalar_generated.h")

foreach(family i8 i16 i32 u8 u16 u32 u64 f32 f64 rune i64 bool string unit)
    foreach(backend vm native vm-native native-vm)
        string(REPLACE "-" "_" name "${backend}")
    endforeach()
endforeach()

foreach(family i8 i16 i32 u8 u16 u32 u64 f32 f64 rune i64 bool string unit)
    foreach(backend vm native vm-native native-vm)
        string(REPLACE "-" "_" name "${backend}")

    endforeach()
endforeach()
