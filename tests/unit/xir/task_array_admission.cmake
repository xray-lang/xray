# Task handles retain their identity when stored in typed owned arrays.
set(host test_xir_task_array_admission)
set(native ${host}_native)
set(generated "${CMAKE_BINARY_DIR}/generated/xir_task_array_admission.c")
add_executable(${host} "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_task_array_admission.c")
target_link_libraries(${host} PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT "${generated}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:${host}> 0 "${generated}"
    DEPENDS ${host} "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_task_array_admission/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_task_array_admission/producer.xr" VERBATIM)
add_executable(${native} "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_task_array_admission.c" "${generated}")
target_link_libraries(${native} PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(${native} PRIVATE XR_TASK_ARRAY_NATIVE=1)
foreach(target ${host} ${native})
    target_compile_definitions(${target} PRIVATE XR_TASK_ARRAY_GROUP=7
        XR_TASK_ARRAY_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_task_array_admission")
    target_include_directories(${target} PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME ${host}_vm COMMAND ${host} 0)
add_test(NAME ${host}_native COMMAND ${native} 1)
add_test(NAME ${host}_mixed_root COMMAND ${native} 2)
add_test(NAME ${host}_mixed_import COMMAND ${native} 3)
set_tests_properties(${host}_vm ${host}_native ${host}_mixed_root ${host}_mixed_import
    PROPERTIES TIMEOUT 120 LABELS "unit;xir;array;ownership;execution;task")
