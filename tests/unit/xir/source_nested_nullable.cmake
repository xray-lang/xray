# Source products replay both packets before emitting this actual whole Program.
set(XIR_SOURCE_NESTED_C ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_source_nested_nullable.c)
add_executable(test_xir_source_nested_nullable xir/test_xir_source_nested_nullable.c)
target_link_libraries(test_xir_source_nested_nullable PRIVATE xray_xir_source_product xray_xir_vm)
target_compile_definitions(test_xir_source_nested_nullable PRIVATE
    XR_SOURCE_NESTED_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_source_nested_nullable")
add_custom_command(OUTPUT ${XIR_SOURCE_NESTED_C}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_source_nested_nullable> ${XIR_SOURCE_NESTED_C}
    DEPENDS test_xir_source_nested_nullable VERBATIM)
foreach(source_nested_mode IN ITEMS native mixed)
    add_executable(test_xir_source_nested_nullable_${source_nested_mode}
        xir/test_xir_source_nested_nullable_execution.c ${XIR_SOURCE_NESTED_C})
    target_link_libraries(test_xir_source_nested_nullable_${source_nested_mode} PRIVATE xray_xir_vm)
    if(source_nested_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_source_nested_nullable_${source_nested_mode} PRIVATE XR_SOURCE_NESTED_MIXED=1)
    else()
        target_compile_definitions(test_xir_source_nested_nullable_${source_nested_mode} PRIVATE XR_SOURCE_NESTED_MIXED=0)
    endif()
endforeach()
foreach(source_nested_target IN ITEMS test_xir_source_nested_nullable
    test_xir_source_nested_nullable_native test_xir_source_nested_nullable_mixed)
    if(MSVC)
        target_compile_options(${source_nested_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${source_nested_target} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${source_nested_target} COMMAND ${source_nested_target})
    set_tests_properties(${source_nested_target} PROPERTIES LABELS "unit;xir;ownership;abi;execution" TIMEOUT 300)
endforeach()
if(WIN32)
    cmake_host_system_information(RESULT XR_SOURCE_METADATA_HOST_THREADS QUERY NUMBER_OF_LOGICAL_CORES)
    set(XR_SOURCE_METADATA_WORKERS 1)
    if(XR_SOURCE_METADATA_HOST_THREADS MATCHES "^[1-9][0-9]*$")
        if(XR_SOURCE_METADATA_HOST_THREADS GREATER 16)
            set(XR_SOURCE_METADATA_WORKERS 16)
        else()
            set(XR_SOURCE_METADATA_WORKERS ${XR_SOURCE_METADATA_HOST_THREADS})
        endif()
    endif()
    add_test(NAME test_xir_source_nested_nullable_metadata
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/xir/run_source_nested_nullable_metadata.py"
            --executable $<TARGET_FILE:test_xir_source_nested_nullable>
            --workers "${XR_SOURCE_METADATA_WORKERS}"
            --reports-root "${CMAKE_CURRENT_BINARY_DIR}/source-nested-metadata-reports")
    set_tests_properties(test_xir_source_nested_nullable_metadata PROPERTIES
        LABELS "unit;xir;ownership;execution" TIMEOUT 300 COST 300
        PROCESSORS "${XR_SOURCE_METADATA_WORKERS}")
    add_test(NAME test_source_metadata_windows_job
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/lib/tests/test_windows_owned_job.py")
    set_tests_properties(test_source_metadata_windows_job PROPERTIES
        LABELS "unit;compiler;ownership;concurrency" TIMEOUT 60 PROCESSORS 1)
else()
    add_test(NAME test_xir_source_nested_nullable_metadata COMMAND test_xir_source_nested_nullable --metadata)
    set_tests_properties(test_xir_source_nested_nullable_metadata PROPERTIES
        LABELS "unit;xir;ownership;execution" TIMEOUT 300)
endif()
