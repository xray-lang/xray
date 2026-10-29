# Keep the scalar interpreter independent of the retired execution libraries.
add_library(xray_register_vm STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/register/xrbc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/register/xrbc_encode.c)
target_include_directories(xray_register_vm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(xray_register_vm PUBLIC xray_native_runtime)

add_executable(test_register_vm ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/test_register_vm.c)
target_link_libraries(test_register_vm PRIVATE xray_register_vm)
add_test(NAME register_vm COMMAND test_register_vm)
set_tests_properties(register_vm PROPERTIES LABELS "unit;vm;fast" TIMEOUT 30)

add_test(NAME register_vm_native_print COMMAND test_register_vm --native-print)
set_tests_properties(register_vm_native_print PROPERTIES
    LABELS "unit;vm;fast" PASS_REGULAR_EXPRESSION "-9223372036854775808" TIMEOUT 10)

add_executable(test_register_vm_alloc
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/test_register_vm_alloc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/register/xrbc_encode.c)
target_include_directories(test_register_vm_alloc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(test_register_vm_alloc PRIVATE xray_native_runtime)
add_test(NAME register_vm_alloc COMMAND test_register_vm_alloc)
set_tests_properties(register_vm_alloc PROPERTIES LABELS "unit;vm;fast" TIMEOUT 10)

add_library(xray_lir_bytecode STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/register/xrbc_lir.c)
target_link_libraries(xray_lir_bytecode PUBLIC xray_register_vm xray_lir xray_native_runtime)
target_compile_features(xray_lir_bytecode PUBLIC c_std_11)
if(MSVC)
    target_compile_options(xray_lir_bytecode PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(xray_lir_bytecode PRIVATE -Wall -Wextra -Werror -pedantic)
endif()

add_executable(test_lir_bytecode ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/test_lir_bytecode.c)
target_link_libraries(test_lir_bytecode PRIVATE xray_lir_bytecode)
add_executable(test_lir_bytecode_alloc
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/test_lir_bytecode_alloc.c)
target_link_libraries(test_lir_bytecode_alloc PRIVATE xray_register_vm xray_lir xray_native_runtime)
add_test(NAME lir_bytecode COMMAND test_lir_bytecode)
add_test(NAME lir_bytecode_alloc COMMAND test_lir_bytecode_alloc)
set_tests_properties(lir_bytecode lir_bytecode_alloc PROPERTIES LABELS "unit;vm;lir;fast" TIMEOUT 30)

add_executable(lir_vm_run ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/lir_vm_run.c)
add_executable(test_lir_bytecode_source
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/test_lir_bytecode_source.c)
target_link_libraries(test_lir_bytecode_source PRIVATE xray_lir_c)
add_executable(test_lir_vm_output
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/test_lir_vm_output.c)
foreach(target lir_vm_run test_lir_bytecode_source test_lir_vm_output)
    target_link_libraries(${target} PRIVATE xray_lir_bytecode xray_lir_lower)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX /utf-8)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
if(UNIX)
    target_link_libraries(test_lir_bytecode_source PRIVATE m)
endif()
add_test(NAME lir_bytecode_source COMMAND test_lir_bytecode_source)
add_test(NAME lir_vm_source COMMAND ${XRAY_PYTHON} -X utf8
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/test_lir_vm_run.py
    --driver $<TARGET_FILE:lir_vm_run> --scratch ${CMAKE_CURRENT_BINARY_DIR}/lir-vm-source)
set_tests_properties(lir_bytecode_source lir_vm_source PROPERTIES LABELS "unit;vm;lir;fast" TIMEOUT 60)
add_test(NAME lir_vm_output COMMAND ${XRAY_PYTHON} -X utf8
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/test_lir_vm_output.py
    --client $<TARGET_FILE:test_lir_vm_output> --logs ${CMAKE_CURRENT_BINARY_DIR}/lir-vm-output)
set_tests_properties(lir_vm_output PROPERTIES LABELS "unit;vm;lir;native-runtime;fast" TIMEOUT 120)

if(MSVC)
    set(skeleton_provider_kind msvc)
else()
    set(skeleton_provider_kind gnu)
endif()
set(skeleton_native_options)
if(ENABLE_ASAN)
    list(APPEND skeleton_native_options --asan)
endif()
add_test(NAME skeleton_diff COMMAND ${XRAY_PYTHON} -X utf8
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/test_skeleton_diff.py
    --vm-driver $<TARGET_FILE:lir_vm_run> --c-driver $<TARGET_FILE:lir_c_run>
    --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/lir/programs
    --compiler "${CMAKE_C_COMPILER}" --kind "${skeleton_provider_kind}"
    --include ${CMAKE_CURRENT_SOURCE_DIR}/include --library ${XRAY_NATIVE_RUNTIME_LINK_INPUTS}
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --work ${CMAKE_CURRENT_BINARY_DIR}/skeleton-diff ${skeleton_native_options})
set_tests_properties(skeleton_diff PROPERTIES LABELS "unit;vm;lir;native;fast" TIMEOUT 120)

add_test(NAME f64_negate_native COMMAND ${XRAY_PYTHON} -X utf8
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/vm/test_f64_negate_native.py
    --driver $<TARGET_FILE:test_lir_bytecode_source> --compiler "${CMAKE_C_COMPILER}"
    --kind "${skeleton_provider_kind}"
    --msvc-runtime "$<TARGET_GENEX_EVAL:xray_native_runtime,$<TARGET_PROPERTY:xray_native_runtime,MSVC_RUNTIME_LIBRARY>>"
    --work ${CMAKE_CURRENT_BINARY_DIR}/f64-negate-native ${skeleton_native_options})
set_tests_properties(f64_negate_native PROPERTIES LABELS "unit;vm;lir;native;fast" TIMEOUT 60)
