

add_executable(test_xir_slice_u8_backend xir/test_xir_slice_u8_backend.c)
target_link_libraries(test_xir_slice_u8_backend PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(slice_u8_backend_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/slice_u8_backend.c")
add_custom_command(OUTPUT "${slice_u8_backend_generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_slice_u8_backend> --emit "${slice_u8_backend_generated}"
    DEPENDS test_xir_slice_u8_backend VERBATIM)
add_executable(test_xir_slice_u8_backend_native xir/test_xir_slice_u8_backend_native.c "${slice_u8_backend_generated}")
target_link_libraries(test_xir_slice_u8_backend_native PRIVATE xray_xir_vm)
foreach(target test_xir_slice_u8_backend test_xir_slice_u8_backend_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
add_test(NAME xir_slice_u8_backend COMMAND test_xir_slice_u8_backend)
add_test(NAME xir_slice_u8_backend_native COMMAND test_xir_slice_u8_backend_native 1)
add_test(NAME xir_slice_u8_backend_vm_to_native COMMAND test_xir_slice_u8_backend_native 2)
add_test(NAME xir_slice_u8_backend_native_to_vm COMMAND test_xir_slice_u8_backend_native 3)
set_tests_properties(xir_slice_u8_backend xir_slice_u8_backend_native xir_slice_u8_backend_vm_to_native
    xir_slice_u8_backend_native_to_vm PROPERTIES LABELS "unit;xir;borrow;view;ownership" TIMEOUT 120)
