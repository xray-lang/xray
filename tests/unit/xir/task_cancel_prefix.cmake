add_executable(test_xir_task_cancel_prefix xir/test_xir_task_cancel_prefix.c)
target_link_libraries(test_xir_task_cancel_prefix PRIVATE xray_xir_vm xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_task_cancel_prefix PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_task_cancel_prefix PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case body call await)
    foreach(outcome ok error)
        add_test(NAME test_xir_task_cancel_measure_${case}_${outcome}
            COMMAND test_xir_task_cancel_prefix --measure ${case} ${outcome})
        set_tests_properties(test_xir_task_cancel_measure_${case}_${outcome}
            PROPERTIES LABELS "unit;xir;task;ownership;runtime" TIMEOUT 60)
    endforeach()
    foreach(control busy cleanup)
        add_test(NAME test_xir_task_cancel_control_${case}_${control}
            COMMAND test_xir_task_cancel_prefix --control ${case} ok ${control})
    endforeach()
    foreach(control before after)
        add_test(NAME test_xir_task_cancel_control_${case}_${control}
            COMMAND test_xir_task_cancel_prefix --control ${case} error ${control})
    endforeach()
endforeach()

foreach(case body call await)
    foreach(api cancel stop)
        add_test(NAME test_xir_task_cancel_boundary_${case}_${api}
            COMMAND test_xir_task_cancel_prefix --prefix ${case} ok ${api} 0)
    endforeach()
endforeach()
add_test(NAME test_xir_task_cancel_boundary_body_terminal_root
    COMMAND test_xir_task_cancel_prefix --prefix body ok cancel 13)
add_test(NAME test_xir_task_cancel_boundary_call_registered
    COMMAND test_xir_task_cancel_prefix --prefix call ok cancel 5)
add_test(NAME test_xir_task_cancel_boundary_await_registered_fault
    COMMAND test_xir_task_cancel_prefix --prefix await error stop 7)
get_property(task_cancel_tests DIRECTORY PROPERTY TESTS)
foreach(name IN LISTS task_cancel_tests)
    if(name MATCHES "^test_xir_task_cancel_(measure|control|boundary)_")
        set_tests_properties(${name} PROPERTIES LABELS "unit;xir;task;ownership;runtime" TIMEOUT 60)
    endif()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/task_cancel_prefix_scan.cmake")
