if(WIN32)
    add_executable(test_native_projection_owner
        ${CMAKE_CURRENT_SOURCE_DIR}/cli/native_projection_owner/test_native_projection_owner.c)
    target_link_libraries(test_native_projection_owner PRIVATE
        xray_xir_native_projection xray_xir_runtime_sdk xray_xir_target)
    target_include_directories(test_native_projection_owner PRIVATE
        ${PROJECT_SOURCE_DIR}/src ${PROJECT_SOURCE_DIR}/include)
    target_compile_definitions(test_native_projection_owner PRIVATE
        NDEBUG WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(test_native_projection_owner PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(test_native_projection_owner PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(test_native_projection_owner PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_native_projection_owner PRIVATE /experimental:c11atomics)
    endif()
    find_program(XIR_PROJECTION_MSVC NAMES cl REQUIRED)
    add_test(NAME projection_actual_binding COMMAND ${XRAY_PYTHON} -X utf8
        ${CMAKE_CURRENT_SOURCE_DIR}/cli/native_projection_owner/test_native_projection_owner.py
        $<TARGET_FILE:test_native_projection_owner> ${PROJECT_SOURCE_DIR}/stdlib
        ${CMAKE_BINARY_DIR}/xir-runtime-sdk ${XIR_PROJECTION_MSVC})
    set_tests_properties(projection_actual_binding PROPERTIES TIMEOUT 300
        RUN_SERIAL TRUE LABELS "unit;xir;ownership;abi;budget")
endif()
