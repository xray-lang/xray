if(NOT WIN32)
    return()
endif()
foreach(mode injected production)
    set(target test_xir_namespace_${mode})
    add_executable(${target} "${CMAKE_CURRENT_LIST_DIR}/namespace_owner/test_xir_namespace.c")
    if(mode STREQUAL "production")
        target_compile_definitions(${target} PRIVATE NAMESPACE_PRODUCTION)
        target_link_libraries(${target} PRIVATE xray_xir_namespace)
    else()
        target_sources(${target} PRIVATE
            "${PROJECT_SOURCE_DIR}/src/base/xutf8.c" "${PROJECT_SOURCE_DIR}/src/base/xsha256.c")
        add_dependencies(${target} xray_xir_namespace)
    endif()
    target_include_directories(${target} PRIVATE
        "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
    target_compile_definitions(${target} PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE /experimental:c11atomics)
    endif()
    add_test(NAME namespace_${mode} COMMAND ${XRAY_PYTHON} -X utf8
        "${CMAKE_CURRENT_LIST_DIR}/namespace_owner/test_xir_namespace.py" $<TARGET_FILE:${target}>)
    set_tests_properties(namespace_${mode} PROPERTIES
        TIMEOUT 180 RUN_SERIAL TRUE LABELS "unit;xir;ownership;budget")
endforeach()
