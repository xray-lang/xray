set(native_runtime_clients)
foreach(kind RANGE 0 6)
    set(client "test_native_runtime_${kind}")
    add_executable(${client} client.c)
    target_compile_definitions(${client} PRIVATE MAP_KIND=${kind})
    target_link_libraries(${client} PRIVATE xray_native_runtime)
    target_include_directories(${client} PRIVATE "${PROJECT_SOURCE_DIR}/src")
    set_target_properties(${client} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${client} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${client} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    list(APPEND native_runtime_clients ${client})
endforeach()
add_custom_target(test_native_runtime_clients DEPENDS ${native_runtime_clients})

# These targets must fail at the real link boundary; they are never in ALL.
add_executable(test_native_runtime_missing EXCLUDE_FROM_ALL link_client.c)
target_link_libraries(test_native_runtime_missing PRIVATE xray_native_runtime)
add_executable(test_native_runtime_duplicate EXCLUDE_FROM_ALL link_client.c map.c map_duplicate.c)
target_link_libraries(test_native_runtime_duplicate PRIVATE xray_native_runtime)

foreach(suite output failures link_contract)
    add_test(NAME native_runtime_${suite}
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_CURRENT_SOURCE_DIR}/check_runtime.py"
            --suite ${suite} --build "${CMAKE_BINARY_DIR}"
            --clients "$<TARGET_FILE:test_native_runtime_0>" "$<TARGET_FILE:test_native_runtime_1>"
                "$<TARGET_FILE:test_native_runtime_2>" "$<TARGET_FILE:test_native_runtime_3>"
                "$<TARGET_FILE:test_native_runtime_4>" "$<TARGET_FILE:test_native_runtime_5>"
                "$<TARGET_FILE:test_native_runtime_6>"
            --logs "${CMAKE_CURRENT_BINARY_DIR}/results-${suite}")
    set_tests_properties(native_runtime_${suite} PROPERTIES
        LABELS "unit;native-runtime;fast" TIMEOUT 90 RUN_SERIAL TRUE)
endforeach()
