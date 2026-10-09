# Five real Source dependency graphs produce canonical C and owned Programs.
set(VIEWS_NATIVE_DIR "${CMAKE_BINARY_DIR}/generated/library_views_native")
set(VIEWS_NATIVE_C)
foreach(_views_index RANGE 0 4)
    list(APPEND VIEWS_NATIVE_C "${VIEWS_NATIVE_DIR}/case${_views_index}.c")
endforeach()
set(VIEWS_NATIVE_H "${VIEWS_NATIVE_DIR}/library_views_native_generated.h")
add_executable(test_xir_library_views_native_producer
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_library_views_native_producer.c")
target_link_libraries(test_xir_library_views_native_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_library_views_native_producer PRIVATE
    XR_ROOT_PARAMETER_NATIVE_FIXTURES="${VIEWS_NATIVE_DIR}/source")
add_custom_command(OUTPUT ${VIEWS_NATIVE_C} "${VIEWS_NATIVE_H}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${VIEWS_NATIVE_DIR}/source"
    COMMAND $<TARGET_FILE:test_xir_library_views_native_producer> --emit ${VIEWS_NATIVE_C} "${VIEWS_NATIVE_H}"
    DEPENDS test_xir_library_views_native_producer
        "${CMAKE_CURRENT_LIST_DIR}/xir_library_views_native_oracles.h"
    VERBATIM)
foreach(_views_kind native mixed)
    set(_views_target "test_xir_library_views_${_views_kind}")
    add_executable(${_views_target} "${CMAKE_CURRENT_LIST_DIR}/test_xir_library_views_native.c"
        ${VIEWS_NATIVE_C} "${VIEWS_NATIVE_H}")
    target_include_directories(${_views_target} PRIVATE "${VIEWS_NATIVE_DIR}")
    target_link_libraries(${_views_target} PRIVATE xray_xir_scalar)
    if(_views_kind STREQUAL "mixed")
        target_compile_definitions(${_views_target} PRIVATE H1_MIXED=1)
        target_link_libraries(${_views_target} PRIVATE xray_xir_vm)
    endif()
    xr_enable_pure_aot_symbol_map(${_views_target})
endforeach()
foreach(_views_target test_xir_library_views_native_producer test_xir_library_views_native test_xir_library_views_mixed)
    set_target_properties(${_views_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${_views_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${_views_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(_views_case diamond leaf disjoint reverse chain17)
    add_test(NAME test_xir_library_views_producer_${_views_case}
        COMMAND test_xir_library_views_native_producer --census ${_views_case})
    set_tests_properties(test_xir_library_views_producer_${_views_case} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;library;compiler;generated-c;ownership")
    foreach(_views_mode native native-vm vm-native)
        if(_views_mode STREQUAL "native")
            set(_views_target test_xir_library_views_native)
        else()
            set(_views_target test_xir_library_views_mixed)
        endif()
        string(REPLACE "-" "_" _views_test_mode "${_views_mode}")
        set(_views_test "test_xir_library_views_${_views_test_mode}_${_views_case}")
        add_test(NAME ${_views_test} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/verify_library_views_native.py"
            --executable $<TARGET_FILE:${_views_target}> --case ${_views_case} --mode ${_views_mode})
        set_tests_properties(${_views_test} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
            LABELS "unit;xir;library;instance;generated-c;ownership;abi")
        foreach(_views_failure error limit)
            add_test(NAME ${_views_test}_typed_${_views_failure}
                COMMAND ${_views_target} --typed-failure ${_views_case} ${_views_mode} ${_views_failure})
            set_tests_properties(${_views_test}_typed_${_views_failure} PROPERTIES
                TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;library;instance;typed-output;ownership")
        endforeach()
    endforeach()
endforeach()
