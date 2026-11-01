add_executable(test_publication_posix test_publication_posix.c)
target_link_libraries(test_publication_posix PRIVATE xray_xir_publication)
target_include_directories(test_publication_posix PRIVATE "${PROJECT_SOURCE_DIR}/src")
set_target_properties(test_publication_posix PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
target_compile_definitions(test_publication_posix PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(test_publication_posix PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME test_publication_posix
    COMMAND test_publication_posix "${CMAKE_CURRENT_BINARY_DIR}/publication-posix-root")
set_tests_properties(test_publication_posix PROPERTIES
    LABELS "unit;cli;publication;filesystem;posix" TIMEOUT 30)
