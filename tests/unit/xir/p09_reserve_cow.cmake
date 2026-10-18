# The core fixture contains the shared value-allocation thread controls.
target_include_directories(test_xir_array_capacity_core PRIVATE "${PROJECT_SOURCE_DIR}/src/base")
if(WIN32)
    target_sources(test_xir_array_capacity_core PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c")
else()
    target_sources(test_xir_array_capacity_core PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c")
    target_link_libraries(test_xir_array_capacity_core PRIVATE Threads::Threads)
endif()
add_test(NAME test_xir_array_reserve_cow COMMAND test_xir_array_capacity_core --cow)
set_tests_properties(test_xir_array_reserve_cow PROPERTIES TIMEOUT 30 LABELS "unit;xir;array;ownership;budget")

set(RESERVE_COW_C "${CMAKE_BINARY_DIR}/generated/xir_p09_reserve.c")
add_executable(test_xir_p09_reserve "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_reserve.c")
target_link_libraries(test_xir_p09_reserve PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${RESERVE_COW_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_p09_reserve> 0 "${RESERVE_COW_C}"
    DEPENDS test_xir_p09_reserve
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity/reserve_cow/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity/reserve_cow/producer.xr" VERBATIM)
add_executable(test_xir_p09_reserve_native "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_p09_reserve.c" "${RESERVE_COW_C}")
target_link_libraries(test_xir_p09_reserve_native PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_p09_reserve_native PRIVATE XR_CAPACITY_NATIVE=1)
foreach(target test_xir_p09_reserve test_xir_p09_reserve_native)
    target_compile_definitions(${target} PRIVATE XR_CAPACITY_COMPONENT="reserve_cow"
        XR_CAPACITY_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_array_capacity")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(mode RANGE 0 3)
    if(mode EQUAL 0)
        set(reserve_cow_target test_xir_p09_reserve)
    else()
        set(reserve_cow_target test_xir_p09_reserve_native)
    endif()
    add_test(NAME test_xir_p09_reserve_${mode} COMMAND ${reserve_cow_target} ${mode})
    set_tests_properties(test_xir_p09_reserve_${mode} PROPERTIES TIMEOUT 120
        LABELS "unit;xir;array;ownership;memory;cancel;execution;budget")
endforeach()
