if(WIN32)
    add_executable(test_xir_host_execution ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_host_execution.c
        ${XIR_ASSERT_PANICS_C} ${XIR_ASSERT_PANICS_MATRIX_C})
    target_link_libraries(test_xir_host_execution PRIVATE xray_xir_runtime_host)
    if(MSVC)
        target_compile_options(test_xir_host_execution PRIVATE /W4 /WX)
    else()
        target_compile_options(test_xir_host_execution PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_xir_host_execution COMMAND test_xir_host_execution)
    set_tests_properties(test_xir_host_execution PROPERTIES LABELS "unit;xir;execution;ownership;sdk;abi" TIMEOUT 180)
    add_executable(test_xir_runtime_sdk ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_runtime_sdk.c
        ${PROJECT_SOURCE_DIR}/src/base/xutf8.c)
    add_dependencies(test_xir_runtime_sdk xir-runtime-sdk-bundle)
    target_include_directories(test_xir_runtime_sdk PRIVATE ${CMAKE_BINARY_DIR}/xir-runtime-sdk)
    target_link_libraries(test_xir_runtime_sdk PRIVATE xray_xir_runtime_sdk)
    if(MSVC)
        target_compile_options(test_xir_runtime_sdk PRIVATE /W4 /WX)
    else()
        target_compile_options(test_xir_runtime_sdk PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME test_xir_runtime_sdk_owner COMMAND test_xir_runtime_sdk
        ${CMAKE_BINARY_DIR}/xir-runtime-sdk ${CMAKE_BINARY_DIR}/xir-runtime-sdk/sdk_manifest.json)
    # The explicit --faults mode retains the complete allocator and IO fault sweep.
    add_test(NAME test_xir_runtime_sdk_shared_resources COMMAND test_xir_runtime_sdk
        ${CMAKE_BINARY_DIR}/xir-runtime-sdk ${CMAKE_BINARY_DIR}/xir-runtime-sdk/sdk_manifest.json --shared-resources)
    set_tests_properties(test_xir_runtime_sdk_shared_resources PROPERTIES
        LABELS "unit;xir;ownership;sdk;resources;target-impact" RUN_SERIAL TRUE TIMEOUT 300 COST 1)
    add_test(NAME test_xir_runtime_sdk_manifest COMMAND ${XRAY_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/sdk_manifest_vectors.py
        --executable $<TARGET_FILE:test_xir_runtime_sdk> --bundle ${CMAKE_BINARY_DIR}/xir-runtime-sdk)
    set_tests_properties(test_xir_runtime_sdk_owner test_xir_runtime_sdk_manifest
        PROPERTIES LABELS "unit;xir;execution;ownership;sdk" RUN_SERIAL TRUE TIMEOUT 300 COST 30)
endif()
