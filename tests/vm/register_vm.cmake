# Keep the scalar interpreter independent of the retired execution libraries.
add_library(xray_register_vm STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/register/xrbc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/register/xrbc_encode.c)
target_include_directories(xray_register_vm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

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
add_test(NAME register_vm_alloc COMMAND test_register_vm_alloc)
set_tests_properties(register_vm_alloc PROPERTIES LABELS "unit;vm;fast" TIMEOUT 10)

add_library(xray_lir_bytecode STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vm/register/xrbc_lir.c)
target_link_libraries(xray_lir_bytecode PUBLIC xray_register_vm xray_lir)
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
target_link_libraries(test_lir_bytecode_alloc PRIVATE xray_register_vm xray_lir)
add_test(NAME lir_bytecode COMMAND test_lir_bytecode)
add_test(NAME lir_bytecode_alloc COMMAND test_lir_bytecode_alloc)
set_tests_properties(lir_bytecode lir_bytecode_alloc PROPERTIES LABELS "unit;vm;lir;fast" TIMEOUT 30)
