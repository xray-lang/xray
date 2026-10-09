add_executable(test_xir_owned_string_source_resources xir/test_xir_owned_string_source_resources.c)
target_link_libraries(test_xir_owned_string_source_resources PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_owned_string_source_resources PRIVATE
    XR_OWNED_STRING_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/owned-string-source-scratch")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/owned-string-source-scratch")
if(MSVC)
    target_compile_options(test_xir_owned_string_source_resources PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_owned_string_source_resources PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case normal census)
    add_test(NAME test_xir_owned_string_source_resources_${case}
        COMMAND test_xir_owned_string_source_resources ${case})
    set_tests_properties(test_xir_owned_string_source_resources_${case} PROPERTIES
        LABELS "unit;xir;source;ownership;runtime;resources" TIMEOUT 60 RUN_SERIAL TRUE)
endforeach()
set(XR_OWNED_STRING_RESOURCE_ORACLE "" CACHE FILEPATH "Independent frozen C2-S semantic70 resource oracle")
if(XR_OWNED_STRING_RESOURCE_ORACLE)
    if(NOT EXISTS "${XR_OWNED_STRING_RESOURCE_ORACLE}")
        message(FATAL_ERROR "C2-S strict oracle must exist and be independently reviewed")
    endif()
    add_test(NAME test_xir_owned_string_source_resources_strict
        COMMAND test_xir_owned_string_source_resources strict "${XR_OWNED_STRING_RESOURCE_ORACLE}")
    set_tests_properties(test_xir_owned_string_source_resources_strict PROPERTIES
        LABELS "unit;xir;source;ownership;runtime;resources" TIMEOUT 60 RUN_SERIAL TRUE)
endif()
