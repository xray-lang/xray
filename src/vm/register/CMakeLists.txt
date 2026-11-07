# Keep the scalar interpreter independent of the retired execution libraries.
add_library(xray_register_vm STATIC
    ${PROJECT_SOURCE_DIR}/src/vm/register/xrbc.c
    ${PROJECT_SOURCE_DIR}/src/vm/register/xrbc_encode.c)
target_include_directories(xray_register_vm PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(xray_register_vm PUBLIC xray_native_runtime)
if(MSVC)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        target_compile_options(xray_register_vm PRIVATE /clang:-ffp-contract=off)
    else()
        target_compile_options(xray_register_vm PRIVATE /fp:precise)
    endif()
else()
    target_compile_options(xray_register_vm PRIVATE -ffp-contract=off)
endif()

add_library(xray_lir_bytecode STATIC
    ${PROJECT_SOURCE_DIR}/src/vm/register/xrbc_lir.c)
target_link_libraries(xray_lir_bytecode PUBLIC xray_register_vm xray_lir xray_native_runtime)
target_compile_features(xray_lir_bytecode PUBLIC c_std_11)
if(MSVC)
    target_compile_options(xray_lir_bytecode PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(xray_lir_bytecode PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
