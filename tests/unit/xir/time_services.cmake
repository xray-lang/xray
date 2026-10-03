# Clock reads, UTC offsets and timer suspension through real Source programs.
if(WIN32)
    add_executable(test_xir_time_services ${CMAKE_CURRENT_LIST_DIR}/time_services/test_time_services.c)
    target_link_libraries(test_xir_time_services PRIVATE xray_cli_source xray_xir_runtime_host bcrypt)
    target_include_directories(test_xir_time_services PRIVATE ${XRAY_COMMON_INCLUDES})
    if(MSVC)
        target_compile_options(test_xir_time_services PRIVATE /W4 /WX /utf-8)
    endif()
    add_test(NAME test_xir_time_services COMMAND test_xir_time_services
        ${PROJECT_SOURCE_DIR}/tests/fixtures/xir_time ${PROJECT_SOURCE_DIR}/stdlib)
    set_tests_properties(test_xir_time_services PROPERTIES TIMEOUT 120
        LABELS "unit;xir;execution;ownership;timer")
endif()
