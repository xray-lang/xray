if(WIN32)
    find_program(P08_TASK_SDK_MSVC NAMES cl)
    find_program(P08_TASK_SDK_CLANG NAMES clang HINTS "$ENV{ProgramFiles}/LLVM/bin")
    foreach(provider msvc clang)
        string(TOUPPER "${provider}" upper)
        if(P08_TASK_SDK_${upper})
            add_test(NAME test_xir_task_constructed_sdk_${provider} COMMAND ${XRAY_PYTHON}
                "${CMAKE_CURRENT_LIST_DIR}/task_constructed_sdk.py"
                --lease $<TARGET_FILE:test_xir_sdk_lease> --emitter $<TARGET_FILE:test_xir_task_constructed_emit>
                --bundle "${CMAKE_BINARY_DIR}/xir-runtime-sdk" --compiler "${P08_TASK_SDK_${upper}}"
                --linker "${XIR_SDK_LINKER}" --provider ${provider}
                --output "${CMAKE_CURRENT_BINARY_DIR}/p08-task-sdk-${provider}")
            set_tests_properties(test_xir_task_constructed_sdk_${provider} PROPERTIES
                TIMEOUT 300 RUN_SERIAL TRUE LABELS "unit;xir;task;sdk;generated-c;portability;ownership")
        endif()
    endforeach()
endif()
