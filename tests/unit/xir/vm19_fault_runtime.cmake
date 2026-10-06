# VM tests observe canonical runtime bodies through the existing finite owner.
foreach(vm19_target IN ITEMS test_xir_vm_pending_exit test_xir_vm_fault_origins test_xir_vm19_fault_oracle)
    add_executable(${vm19_target} xir/${vm19_target}.c)
    target_include_directories(${vm19_target} PRIVATE ${XRAY_COMMON_INCLUDES}
        "${CMAKE_CURRENT_SOURCE_DIR}/xir" "${CMAKE_CURRENT_SOURCE_DIR}/xir/vm19_fault")
    target_link_libraries(${vm19_target} PRIVATE xray_xir_vm)
    if(MSVC)
        target_compile_options(${vm19_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${vm19_target} PRIVATE -Wall -Wextra -Werror)
    endif()
    if(UNIX)
        target_link_libraries(${vm19_target} PRIVATE m)
    endif()
endforeach()
foreach(vm19_normal IN ITEMS test_xir_vm_pending_exit test_xir_vm_fault_origins)
    add_test(NAME ${vm19_normal} COMMAND $<TARGET_FILE:${vm19_normal}>)
    set_tests_properties(${vm19_normal} PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE PROCESSORS 1
        LABELS "unit;xir;task;runtime;ownership")
endforeach()
foreach(vm19_kind IN ITEMS baseline matrix)
    add_test(NAME test_xir_vm19_runtime_${vm19_kind}
        COMMAND ${XRAY_PYTHON} "${CMAKE_CURRENT_SOURCE_DIR}/xir/vm19_fault/run_vm19_registered.py"
        --kind ${vm19_kind} --exe $<TARGET_FILE:test_xir_vm19_fault_oracle>
        --source-root "${CMAKE_SOURCE_DIR}" --build-root "${CMAKE_BINARY_DIR}"
        --reports-root "${CMAKE_CURRENT_BINARY_DIR}/vm19-runtime-${vm19_kind}-reports")
    # The parent preserves sixty seconds per real child, with serial execution.
    set_tests_properties(test_xir_vm19_runtime_${vm19_kind} PROPERTIES RUN_SERIAL TRUE PROCESSORS 1
        LABELS "unit;xir;task;runtime;ownership;resource-budget")
endforeach()
