if(WIN32)
    # Exercise the actual MD source archive closure with the original exhaustive facade matrix.
    add_executable(test_source_sdk_faults
        "${PROJECT_SOURCE_DIR}/tests/unit/api/source_public/test_source_public_faults.c")
    target_link_libraries(test_source_sdk_faults PRIVATE ${XRAY_SOURCE_SDK_ARCHIVE_TARGETS})
    target_include_directories(test_source_sdk_faults PRIVATE "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/unit")
    target_compile_definitions(test_source_sdk_faults PRIVATE XR_PUBLIC_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
    set_target_properties(test_source_sdk_faults PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreadedDLL
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(test_source_sdk_faults PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(test_source_sdk_faults PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    add_test(NAME source_sdk_faults COMMAND test_source_sdk_faults)
    set_tests_properties(source_sdk_faults PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE COST 100
        LABELS "unit;api;source;sdk;ownership;compiler-faults;runtime-faults")
endif()
if(WIN32 AND NOT ENABLE_ASAN AND NOT ENABLE_SANITIZERS AND NOT ENABLE_UBSAN
    AND NOT ENABLE_TSAN AND NOT ENABLE_MSAN)
    add_test(NAME source_sdk_installed COMMAND ${XRAY_PYTHON} -B
        "${CMAKE_CURRENT_LIST_DIR}/source_sdk_install.py"
        --build "${CMAKE_BINARY_DIR}" --compiler "${CMAKE_C_COMPILER}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/source-sdk-installed")
    set_tests_properties(source_sdk_installed PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE COST 30 LABELS "unit;api;source;sdk;install;ownership")
endif()
