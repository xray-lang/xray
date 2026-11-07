

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/owned-string-source-scratch")
set(XR_OWNED_STRING_RESOURCE_ORACLE "" CACHE FILEPATH "Independent frozen C2-S semantic70 resource oracle")
if(XR_OWNED_STRING_RESOURCE_ORACLE)
    if(NOT EXISTS "${XR_OWNED_STRING_RESOURCE_ORACLE}")
        message(FATAL_ERROR "C2-S strict oracle must exist and be independently reviewed")
    endif()

    set_tests_properties(test_xir_owned_string_source_resources_strict PROPERTIES
        LABELS "unit;xir;source;ownership;runtime;resources" TIMEOUT 60 RUN_SERIAL TRUE)
endif()
