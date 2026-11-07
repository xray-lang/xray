


set(intrinsic_contains_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/intrinsic_contains.c")





set(XIR_TYPED_DISPLAY_C ${CMAKE_CURRENT_BINARY_DIR}/typed-display/received.c)















set(from_utf8_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/from_utf8.c")










# Physical observer is a distinct consumer; canonical API/native targets keep their ordinary provider.



















set(module_limits_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/module-limits.c")










add_executable(test_xir_pointer_owner xir/test_xir_pointer_owner.c)
target_link_libraries(test_xir_pointer_owner PRIVATE xray_xir_scalar)
set_target_properties(test_xir_pointer_owner PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_xir_pointer_owner PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_pointer_owner PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME xir_pointer_owner COMMAND test_xir_pointer_owner)
set_tests_properties(xir_pointer_owner PROPERTIES LABELS "unit;xir;ownership" TIMEOUT 60)
add_executable(test_xir_pointer_owner_fault xir/test_xir_pointer_owner.c xir/xir_pointer_owner_probe.c)
target_link_libraries(test_xir_pointer_owner_fault PRIVATE xray_xir_scalar)
target_compile_definitions(test_xir_pointer_owner_fault PRIVATE XR_POINTER_FAULT_CASE=1)
set_target_properties(test_xir_pointer_owner_fault PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_xir_pointer_owner_fault PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_xir_pointer_owner_fault PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_xir_pointer_owner_fault PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME xir_pointer_owner_fault COMMAND test_xir_pointer_owner_fault)
set_tests_properties(xir_pointer_owner_fault PROPERTIES LABELS "unit;xir;ownership" TIMEOUT 60)







set(buffer_storage_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/buffer_storage.c")















foreach(display_part source values)
    add_executable(test_xir_typed_display_${display_part} xir/test_xir_typed_display_${display_part}.c)
    target_link_libraries(test_xir_typed_display_${display_part} PRIVATE xray_xir_source  )
    set_target_properties(test_xir_typed_display_${display_part} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(test_xir_typed_display_${display_part} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(test_xir_typed_display_${display_part} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    if(display_part STREQUAL "source")
        add_test(NAME xir_typed_display_${display_part} COMMAND test_xir_typed_display_${display_part} ${PROJECT_SOURCE_DIR})
    else()
        add_test(NAME xir_typed_display_${display_part} COMMAND test_xir_typed_display_${display_part})
    endif()
    set_tests_properties(xir_typed_display_${display_part} PROPERTIES LABELS "unit;xir;ownership" TIMEOUT 60)
endforeach()

add_executable(test_xir_mem_borrow_source xir/test_xir_mem_borrow_source.c)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mem-borrow-cold")
target_compile_definitions(test_xir_mem_borrow_source PRIVATE XR_MEM_COLD_ROOT="${CMAKE_CURRENT_BINARY_DIR}/mem-borrow-cold")
target_link_libraries(test_xir_mem_borrow_source PRIVATE xray_xir_source)
set_target_properties(test_xir_mem_borrow_source PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_xir_mem_borrow_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_mem_borrow_source PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME xir_mem_borrow_source COMMAND test_xir_mem_borrow_source ${PROJECT_SOURCE_DIR})
set_tests_properties(xir_mem_borrow_source PROPERTIES LABELS "unit;xir;ownership" TIMEOUT 60)
