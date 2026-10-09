# Nine shared typed Source programs produce canonical C and owned Programs.
set(TYPED_NATIVE_DIR "${CMAKE_BINARY_DIR}/generated/library_typed_native")
set(TYPED_NATIVE_C)
foreach(_typed_index RANGE 0 8)
    list(APPEND TYPED_NATIVE_C "${TYPED_NATIVE_DIR}/case${_typed_index}.c")
endforeach()
set(TYPED_NATIVE_H "${TYPED_NATIVE_DIR}/library_typed_native_generated.h")
add_executable(test_xir_library_typed_native_producer
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_library_typed_native_producer.c")
target_link_libraries(test_xir_library_typed_native_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_library_typed_native_producer PRIVATE
    XR_ROOT_PARAMETER_NATIVE_FIXTURES="${TYPED_NATIVE_DIR}/source")
add_custom_command(OUTPUT ${TYPED_NATIVE_C} "${TYPED_NATIVE_H}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TYPED_NATIVE_DIR}/source"
    COMMAND $<TARGET_FILE:test_xir_library_typed_native_producer> --emit ${TYPED_NATIVE_C} "${TYPED_NATIVE_H}"
    DEPENDS test_xir_library_typed_native_producer
        "${CMAKE_CURRENT_LIST_DIR}/xir_library_typed_native_oracles.h"
        "${CMAKE_CURRENT_LIST_DIR}/xir_library_typed_programs.h"
    VERBATIM)
foreach(_typed_kind native mixed)
    set(_typed_target "test_xir_library_typed_${_typed_kind}")
    add_executable(${_typed_target} "${CMAKE_CURRENT_LIST_DIR}/test_xir_library_typed_native.c"
        ${TYPED_NATIVE_C} "${TYPED_NATIVE_H}")
    target_include_directories(${_typed_target} PRIVATE "${TYPED_NATIVE_DIR}")
    target_link_libraries(${_typed_target} PRIVATE xray_xir_scalar)
    if(_typed_kind STREQUAL "mixed")
        target_compile_definitions(${_typed_target} PRIVATE H1_MIXED=1)
        target_link_libraries(${_typed_target} PRIVATE xray_xir_vm)
    endif()
    xr_enable_pure_aot_symbol_map(${_typed_target})
endforeach()
foreach(_typed_target test_xir_library_typed_native_producer test_xir_library_typed_native test_xir_library_typed_mixed)
    set_target_properties(${_typed_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${_typed_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${_typed_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(_typed_case typed_nominal typed_interface construction generic_parent_lr generic_parent_rl member_witness carrier_member enum_owned class_owned)
    add_test(NAME test_xir_library_typed_producer_${_typed_case}
        COMMAND test_xir_library_typed_native_producer --census ${_typed_case})
    set_tests_properties(test_xir_library_typed_producer_${_typed_case} PROPERTIES
        TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;library;compiler;generated-c;ownership")
    foreach(_typed_mode native vm native-vm vm-native)
        if(_typed_mode STREQUAL "native")
            set(_typed_target test_xir_library_typed_native)
        else()
            set(_typed_target test_xir_library_typed_mixed)
        endif()
        string(REPLACE "-" "_" _typed_test_mode "${_typed_mode}")
        set(_typed_test "test_xir_library_typed_${_typed_test_mode}_${_typed_case}")
        add_test(NAME ${_typed_test} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/verify_library_typed_native.py"
            --executable $<TARGET_FILE:${_typed_target}> --case ${_typed_case} --mode ${_typed_mode})
        set_tests_properties(${_typed_test} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
            LABELS "unit;xir;library;instance;generated-c;ownership;abi")
        foreach(_typed_failure error limit)
            add_test(NAME ${_typed_test}_typed_${_typed_failure}
                COMMAND ${_typed_target} --typed-failure ${_typed_case} ${_typed_mode} ${_typed_failure})
            set_tests_properties(${_typed_test}_typed_${_typed_failure} PROPERTIES
                TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1 LABELS "unit;xir;library;instance;typed-output;ownership")
        endforeach()
    endforeach()
endforeach()
