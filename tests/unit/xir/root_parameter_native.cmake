# The producer owns Source, Checked replay, specialization and canonical CGen.
set(H1_NATIVE_DIR "${CMAKE_BINARY_DIR}/generated/root_parameter_native")
set(H1_NATIVE_C)
foreach(_h1_index RANGE 0 3)
    list(APPEND H1_NATIVE_C "${H1_NATIVE_DIR}/case${_h1_index}.c")
endforeach()
set(H1_NATIVE_H "${H1_NATIVE_DIR}/root_parameter_native_generated.h")
add_executable(test_xir_root_parameter_native_producer
    "${CMAKE_CURRENT_LIST_DIR}/test_xir_root_parameter_native_producer.c")
target_link_libraries(test_xir_root_parameter_native_producer PRIVATE xray_xir_source xray_xir_cgen)
target_compile_definitions(test_xir_root_parameter_native_producer PRIVATE
    XR_ROOT_PARAMETER_NATIVE_FIXTURES="${H1_NATIVE_DIR}/source")
add_custom_command(OUTPUT ${H1_NATIVE_C} "${H1_NATIVE_H}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${H1_NATIVE_DIR}/source"
    COMMAND $<TARGET_FILE:test_xir_root_parameter_native_producer> ${H1_NATIVE_C} "${H1_NATIVE_H}"
    DEPENDS test_xir_root_parameter_native_producer
        "${CMAKE_CURRENT_LIST_DIR}/xir_root_parameter_oracles.h"
    VERBATIM)
foreach(_h1_kind native mixed)
    set(_h1_target "test_xir_root_parameter_${_h1_kind}")
    add_executable(${_h1_target} "${CMAKE_CURRENT_LIST_DIR}/test_xir_root_parameter_native.c"
        ${H1_NATIVE_C} "${H1_NATIVE_H}")
    target_include_directories(${_h1_target} PRIVATE "${H1_NATIVE_DIR}")
    target_link_libraries(${_h1_target} PRIVATE xray_xir_scalar)
    if(_h1_kind STREQUAL "mixed")
        target_compile_definitions(${_h1_target} PRIVATE H1_MIXED=1)
        target_link_libraries(${_h1_target} PRIVATE xray_xir_vm)
    endif()
    xr_enable_pure_aot_symbol_map(${_h1_target})
endforeach()
foreach(_h1_target test_xir_root_parameter_native_producer test_xir_root_parameter_native test_xir_root_parameter_mixed)
    set_target_properties(${_h1_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${_h1_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${_h1_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(_h1_source apply fixed root forward)
    foreach(_h1_mode native native-vm vm-native)
        if(_h1_mode STREQUAL "native")
            set(_h1_target test_xir_root_parameter_native)
        else()
            set(_h1_target test_xir_root_parameter_mixed)
        endif()
        string(REPLACE "-" "_" _h1_test_mode "${_h1_mode}")
        set(_h1_test "test_xir_root_parameter_${_h1_test_mode}_${_h1_source}")
        add_test(NAME ${_h1_test} COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/verify_root_parameter_native.py"
            --executable $<TARGET_FILE:${_h1_target}> --case "${_h1_source}.xr" --mode "${_h1_mode}")
        set_tests_properties(${_h1_test} PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE PROCESSORS 1
            LABELS "unit;xir;higher-order;source;root-effects;ownership;instance;generated-c;portability;abi")
    endforeach()
endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/root_parameter_native_resources.cmake")
