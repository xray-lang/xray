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
    add_xray_bootstrap_executable(test_xir_sdk_lease ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_sdk_lease.c)
    target_link_libraries(test_xir_sdk_lease PRIVATE xray_xir_runtime_sdk)
    add_xray_bootstrap_executable(test_xir_runtime_sdk ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_runtime_sdk.c)
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
    add_test(NAME test_xir_runtime_sdk_manifest COMMAND ${XRAY_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/sdk_manifest_vectors.py
        --executable $<TARGET_FILE:test_xir_runtime_sdk> --bundle ${CMAKE_BINARY_DIR}/xir-runtime-sdk)
    add_test(NAME test_xir_runtime_sdk_install COMMAND ${XRAY_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/sdk_install.py --build ${CMAKE_BINARY_DIR}
        --owner $<TARGET_FILE:test_xir_runtime_sdk>)
    add_test(NAME test_xir_runtime_sdk_wire_vector COMMAND ${XRAY_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_sdk_identity.py)
    set_tests_properties(test_xir_runtime_sdk_owner test_xir_runtime_sdk_manifest test_xir_runtime_sdk_install
        test_xir_runtime_sdk_wire_vector
        PROPERTIES LABELS "unit;xir;execution;ownership;sdk;abi" RUN_SERIAL TRUE TIMEOUT 300 COST 30)
    find_program(XIR_SDK_LINKER NAMES link REQUIRED)
    find_program(XIR_SDK_MSVC NAMES cl)
    find_program(XIR_SDK_CLANG NAMES clang)
    find_program(XIR_SDK_ZIG NAMES zig)
    foreach(provider msvc clang zig)
        string(TOUPPER ${provider} upper)
        if(XIR_SDK_${upper})
            add_test(NAME test_xir_runtime_sdk_${provider}_native COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/xir/sdk_native_consumers.py
                --lease $<TARGET_FILE:test_xir_sdk_lease> --generator $<TARGET_FILE:test_xir_source_product>
                --bundle ${CMAKE_BINARY_DIR}/xir-runtime-sdk --stdlib ${PROJECT_SOURCE_DIR}/stdlib
                --compiler ${XIR_SDK_${upper}} --linker ${XIR_SDK_LINKER} --provider ${provider})
            set_tests_properties(test_xir_runtime_sdk_${provider}_native
                PROPERTIES LABELS "unit;xir;execution;ownership;sdk;abi;generated-c;portability"
                RUN_SERIAL TRUE TIMEOUT 300 COST 30)
        endif()
    endforeach()
endif()
