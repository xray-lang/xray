set(f64_transport_dir "${CMAKE_CURRENT_LIST_DIR}")
set(f64_transport_generated "${CMAKE_BINARY_DIR}/generated/source-f64-transport")
add_executable(test_source_product_source_f64_transport_emitter "${f64_transport_dir}/test_source_product_source_f64_transport.c")
add_custom_command(OUTPUT "${f64_transport_generated}/carrier.c"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${f64_transport_generated}"
    COMMAND $<TARGET_FILE:test_source_product_source_f64_transport_emitter> "${f64_transport_dir}" "${f64_transport_dir}/source_f64_transport.xr" emit "${f64_transport_generated}/carrier.c"
    DEPENDS test_source_product_source_f64_transport_emitter "${f64_transport_dir}/source_f64_transport.xr" VERBATIM)
add_executable(test_source_product_source_f64_transport "${f64_transport_dir}/test_source_product_source_f64_transport.c"
    "${f64_transport_generated}/carrier.c")
target_compile_definitions(test_source_product_source_f64_transport PRIVATE XR_F64_NATIVE=1)
foreach(target IN ITEMS test_source_product_source_f64_transport_emitter test_source_product_source_f64_transport)
    target_link_libraries(${target} PRIVATE xray_xir_source_product)
    target_include_directories(${target} PRIVATE "${f64_transport_dir}/..")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
foreach(mode IN ITEMS vm native native-to-vm vm-to-native)
    add_test(NAME source_product_source_f64_transport_${mode} COMMAND test_source_product_source_f64_transport "${f64_transport_dir}" "${f64_transport_dir}/source_f64_transport.xr" ${mode})
    set_tests_properties(source_product_source_f64_transport_${mode} PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;program-consumer;f64;ownership")
endforeach()
