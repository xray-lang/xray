add_executable(test_xir_compile_domain "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_compile_domain.c")
target_include_directories(test_xir_compile_domain PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base")
target_link_libraries(test_xir_compile_domain PRIVATE xray_xir_scalar)
if(WIN32)
    target_sources(test_xir_compile_domain PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c")
else()
    target_sources(test_xir_compile_domain PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c")
    target_link_libraries(test_xir_compile_domain PRIVATE Threads::Threads)
endif()
set_target_properties(test_xir_compile_domain PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_compile_domain PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_compile_domain PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_compile_domain COMMAND test_xir_compile_domain)
set_tests_properties(test_xir_compile_domain PROPERTIES TIMEOUT 120 LABELS "unit;xir;ctfe;ownership;memory;budget")









add_executable(test_xir_compile_domain_owned "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_compile_domain_owned.c")
target_include_directories(test_xir_compile_domain_owned PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/src/base")
target_link_libraries(test_xir_compile_domain_owned PRIVATE xray_xir_scalar)
if(WIN32)
    target_sources(test_xir_compile_domain_owned PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/thread_win.c")
else()
    target_sources(test_xir_compile_domain_owned PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/thread_unix.c")
    target_link_libraries(test_xir_compile_domain_owned PRIVATE Threads::Threads)
endif()
set_target_properties(test_xir_compile_domain_owned PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_compile_domain_owned PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_compile_domain_owned PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_compile_domain_owned COMMAND test_xir_compile_domain_owned)
set_tests_properties(test_xir_compile_domain_owned PROPERTIES TIMEOUT 120 LABELS "unit;xir;ctfe;ownership;execution")
