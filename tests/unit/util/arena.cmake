# General allocator tests use the explicit system backing without the compiler runtime.
get_filename_component(XR_UTIL_ARENA_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../.." ABSOLUTE)
add_executable(test_arena
    "${CMAKE_CURRENT_LIST_DIR}/test_arena.c"
    "${XR_UTIL_ARENA_ROOT}/src/base/xarena.c"
    "${XR_UTIL_ARENA_ROOT}/src/base/xarena_backing.c")
target_include_directories(test_arena PRIVATE "${XR_UTIL_ARENA_ROOT}/src")
set_target_properties(test_arena PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_definitions(test_arena PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(test_arena PRIVATE /W4 /WX /utf-8)
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(test_arena PRIVATE /experimental:c11atomics)
    endif()
else()
    target_compile_options(test_arena PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
add_test(NAME test_arena COMMAND test_arena)
set_tests_properties(test_arena PROPERTIES LABELS "unit;util;arena;ownership")
