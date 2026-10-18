add_executable(test_xir_mixed_iterator_producer xir/test_xir_mixed_iterator_producer.c)
target_link_libraries(test_xir_mixed_iterator_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_mixed_iterator_producer PRIVATE
    XR_ITERATOR_LIFETIME_FIXTURES="${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_iterators"
    XR_SOURCE_STDLIB="${PROJECT_SOURCE_DIR}/stdlib")
set(XIR_MIXED_ITERATOR_LIFETIME_C "${CMAKE_CURRENT_BINARY_DIR}/generated/xir_mixed_iterator_lifetime.c")
add_custom_command(OUTPUT "${XIR_MIXED_ITERATOR_LIFETIME_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_mixed_iterator_producer> "${XIR_MIXED_ITERATOR_LIFETIME_C}"
    DEPENDS test_xir_mixed_iterator_producer
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_iterators/root.xr"
        "${PROJECT_SOURCE_DIR}/tests/fixtures/xir_p20_iterators/worker.xr"
    VERBATIM)
add_executable(test_xir_mixed_iterator_lifetime xir/test_xir_mixed_iterator_lifetime.c "${XIR_MIXED_ITERATOR_LIFETIME_C}")
target_link_libraries(test_xir_mixed_iterator_lifetime PRIVATE xray_xir_vm xray_xir_scalar)
foreach(iterator_lifetime_target IN ITEMS test_xir_mixed_iterator_producer test_xir_mixed_iterator_lifetime)
    set_target_properties(${iterator_lifetime_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${iterator_lifetime_target} PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(${iterator_lifetime_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(iterator_lifetime_backend IN ITEMS vm-native native-vm vm native)
    add_test(NAME test_xir_mixed_iterator_lifetime_${iterator_lifetime_backend}
        COMMAND test_xir_mixed_iterator_lifetime ${iterator_lifetime_backend})
    set_tests_properties(test_xir_mixed_iterator_lifetime_${iterator_lifetime_backend} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE COST 5 LABELS "unit;xir;mixed;ownership;generated-c;lifecycle;generator;iterator;cell;timer;cleanup")
endforeach()
