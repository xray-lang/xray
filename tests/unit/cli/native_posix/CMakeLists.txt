# The owners behind `xray build` on macOS and Linux, driven directly.
add_executable(test_native_posix test_native_posix.c)
target_link_libraries(test_native_posix PRIVATE xray_xir_workspace
    xray_toolchain_process xray_xir_target)
target_include_directories(test_native_posix PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/include")
set_target_properties(test_native_posix PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(test_native_posix PRIVATE _POSIX_C_SOURCE=200809L)
if(APPLE)
    target_compile_definitions(test_native_posix PRIVATE _DARWIN_C_SOURCE)
endif()
target_compile_options(test_native_posix PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME test_native_posix
    COMMAND test_native_posix "${CMAKE_CURRENT_BINARY_DIR}/native-posix-root")
set_tests_properties(test_native_posix PROPERTIES
    LABELS "unit;cli;native;filesystem;posix" TIMEOUT 120)
