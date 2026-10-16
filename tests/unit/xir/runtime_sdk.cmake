include("${CMAKE_CURRENT_LIST_DIR}/source_sdk.cmake")
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
    add_executable(test_xir_sdk_lease ${CMAKE_CURRENT_SOURCE_DIR}/xir/test_xir_sdk_lease.c)
    target_link_libraries(test_xir_sdk_lease PRIVATE xray_xir_runtime_sdk)
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
    # Target-only impact gate; SDK loader/parser changes still require the full owner matrix.
    add_test(NAME test_xir_runtime_sdk_shared_resources COMMAND test_xir_runtime_sdk
        ${CMAKE_BINARY_DIR}/xir-runtime-sdk ${CMAKE_BINARY_DIR}/xir-runtime-sdk/sdk_manifest.json --shared-resources)
    set_tests_properties(test_xir_runtime_sdk_shared_resources PROPERTIES
        LABELS "unit;xir;ownership;sdk;resources;target-impact" RUN_SERIAL TRUE TIMEOUT 300 COST 1)
    add_test(NAME test_xir_runtime_sdk_manifest COMMAND ${XRAY_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/sdk_manifest_vectors.py
        --executable $<TARGET_FILE:test_xir_runtime_sdk> --bundle ${CMAKE_BINARY_DIR}/xir-runtime-sdk)
    set(XIR_SDK_INSTALL_OPTIONS)
    if(TARGET source-sdk-bundle)
        list(APPEND XIR_SDK_INSTALL_OPTIONS --source-sdk)
    endif()
    add_test(NAME test_xir_runtime_sdk_install COMMAND ${XRAY_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/sdk_install.py --build ${CMAKE_BINARY_DIR}
        --owner $<TARGET_FILE:test_xir_runtime_sdk>
        --prefix "${CMAKE_BINARY_DIR}/tests/unit/sdk-install-current-case" ${XIR_SDK_INSTALL_OPTIONS})
    add_test(NAME test_xir_runtime_sdk_wire_vector COMMAND ${XRAY_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_sdk_identity.py)
    add_test(NAME test_xir_runtime_sdk_current_wire_vector COMMAND ${XRAY_PYTHON}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_sdk_current_identity.py)
    set_tests_properties(test_xir_runtime_sdk_owner test_xir_runtime_sdk_manifest test_xir_runtime_sdk_install
        test_xir_runtime_sdk_wire_vector test_xir_runtime_sdk_current_wire_vector
        PROPERTIES LABELS "unit;xir;execution;ownership;sdk;abi" RUN_SERIAL TRUE TIMEOUT 300 COST 30)
    # The exhaustive instrumented fault matrix takes substantially longer than its Release lane.
    if(ENABLE_ASAN OR ENABLE_SANITIZERS)
        set_tests_properties(test_xir_runtime_sdk_owner PROPERTIES COST 210)
    endif()
    find_program(XIR_SDK_LINKER NAMES link REQUIRED)
    find_program(XIR_SDK_MSVC NAMES cl)
    find_program(XIR_SDK_CLANG NAMES clang)
    find_program(XIR_SDK_ZIG NAMES zig)
    include("${CMAKE_CURRENT_LIST_DIR}/sdk_consumer_admission.cmake")
    if(XIR_SDK_MSVC)
        xir_register_sdk_consumer_admission("${XRAY_PYTHON}" "$<TARGET_FILE:test_xir_sdk_lease>"
            "${CMAKE_BINARY_DIR}/xir-runtime-sdk" "${XIR_SDK_MSVC}" "${XIR_SDK_LINKER}")
    endif()
    foreach(provider msvc clang zig)
        string(TOUPPER ${provider} upper)
        if(XIR_SDK_${upper})
            if(NOT provider STREQUAL "zig")
                add_test(NAME test_xir_runtime_sdk_${provider}_archive_closure COMMAND ${XRAY_PYTHON} -B
                    ${CMAKE_CURRENT_SOURCE_DIR}/xir/sdk_archive_closure.py
                    --build ${CMAKE_BINARY_DIR} --compiler ${XIR_SDK_${upper}}
                    --linker ${XIR_SDK_LINKER} --archiver ${CMAKE_AR} --provider ${provider}
                    --output ${CMAKE_CURRENT_BINARY_DIR}/sdk-archive-closure)
                set_tests_properties(test_xir_runtime_sdk_${provider}_archive_closure PROPERTIES
                    LABELS "unit;xir;ownership;sdk;abi;archive;closure" RUN_SERIAL TRUE TIMEOUT 300 COST 3)
            endif()
            add_test(NAME test_xir_runtime_sdk_${provider}_native COMMAND ${XRAY_PYTHON}
                ${CMAKE_CURRENT_SOURCE_DIR}/xir/sdk_native_consumers.py
                --lease $<TARGET_FILE:test_xir_sdk_lease> --generator $<TARGET_FILE:test_xir_source_product_linked>
                --original-generator $<TARGET_FILE:test_xir_source_product>
                --bundle ${CMAKE_BINARY_DIR}/xir-runtime-sdk --stdlib ${PROJECT_SOURCE_DIR}/stdlib
                --compiler ${XIR_SDK_${upper}} --linker ${XIR_SDK_LINKER} --provider ${provider})
            set_tests_properties(test_xir_runtime_sdk_${provider}_native
                PROPERTIES LABELS "unit;xir;execution;ownership;sdk;abi;generated-c;portability"
                RUN_SERIAL TRUE TIMEOUT 300 COST 30)
            add_test(NAME test_xir_runtime_sdk_${provider}_host
                COMMAND ${XRAY_PYTHON} -B ${CMAKE_CURRENT_LIST_DIR}/sdk_source_host_consumers.py
                    --lease $<TARGET_FILE:test_xir_sdk_lease> --generator $<TARGET_FILE:test_xir_host_call_source>
                    --bundle ${CMAKE_BINARY_DIR}/xir-runtime-sdk --compiler ${XIR_SDK_${upper}}
                    --linker ${XIR_SDK_LINKER} --provider ${provider}
                    --output ${CMAKE_CURRENT_BINARY_DIR}/sdk-source-host-evidence)
            set_tests_properties(test_xir_runtime_sdk_${provider}_host PROPERTIES
                LABELS "unit;xir;execution;ownership;sdk;abi;generated-c;portability;host"
                RUN_SERIAL TRUE TIMEOUT 300 COST 30)
        endif()
    endforeach()
endif()
