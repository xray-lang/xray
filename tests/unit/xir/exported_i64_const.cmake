add_executable(test_xir_exported_i64_const "${CMAKE_CURRENT_LIST_DIR}/test_xir_exported_i64_const.c")
target_link_libraries(test_xir_exported_i64_const PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
set(XR_EXPORTED_I64_CONST_WORK "${CMAKE_CURRENT_BINARY_DIR}/exported-i64-const-value-work")
file(MAKE_DIRECTORY "${XR_EXPORTED_I64_CONST_WORK}/source" "${XR_EXPORTED_I64_CONST_WORK}/cold")
target_compile_definitions(test_xir_exported_i64_const PRIVATE XR_EXPORTED_I64_CONST_WORK="${XR_EXPORTED_I64_CONST_WORK}")
set_target_properties(test_xir_exported_i64_const PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_exported_i64_const PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_exported_i64_const PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_exported_i64_const COMMAND test_xir_exported_i64_const)
add_test(NAME test_xir_exported_i64_const_authority COMMAND test_xir_exported_i64_const --authority)
set_tests_properties(test_xir_exported_i64_const PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;const;library;ownership")

set(XR_EXPORTED_I64_CONST_SOURCE_PACKET "${CMAKE_CURRENT_BINARY_DIR}/generated/exported-i64-const-source.chk")
set(XR_EXPORTED_I64_CONST_CATALOG_PACKET "${CMAKE_CURRENT_BINARY_DIR}/generated/exported-i64-const-catalog.chk")
add_custom_command(OUTPUT "${XR_EXPORTED_I64_CONST_SOURCE_PACKET}" "${XR_EXPORTED_I64_CONST_CATALOG_PACKET}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_exported_i64_const> "${XR_EXPORTED_I64_CONST_SOURCE_PACKET}" "${XR_EXPORTED_I64_CONST_CATALOG_PACKET}"
    DEPENDS test_xir_exported_i64_const VERBATIM)
add_executable(test_xir_exported_i64_const_packet "${CMAKE_CURRENT_LIST_DIR}/test_xir_exported_i64_const_execution.c")
target_compile_definitions(test_xir_exported_i64_const_packet PRIVATE CI_CONSUMER_KIND=0)
target_link_libraries(test_xir_exported_i64_const_packet PRIVATE xray_xir_vm xray_xir_cgen)
set(XR_EXPORTED_I64_CONST_TARGETS test_xir_exported_i64_const_packet)
foreach(const_flow IN ITEMS source catalog)
    if(const_flow STREQUAL "source")
        set(const_packet "${XR_EXPORTED_I64_CONST_SOURCE_PACKET}")
    else()
        set(const_packet "${XR_EXPORTED_I64_CONST_CATALOG_PACKET}")
    endif()
    set(const_c "${CMAKE_CURRENT_BINARY_DIR}/generated/exported-i64-const-${const_flow}.c")
    add_custom_command(OUTPUT "${const_c}"
        COMMAND $<TARGET_FILE:test_xir_exported_i64_const_packet> "${const_packet}" "${const_c}"
        DEPENDS test_xir_exported_i64_const_packet "${const_packet}" VERBATIM)
    add_test(NAME test_xir_exported_i64_const_${const_flow}_vm COMMAND test_xir_exported_i64_const_packet
        "${const_packet}" "${XR_EXPORTED_I64_CONST_WORK}/${const_flow}-vm.c")
    foreach(const_mode IN ITEMS native vm_native native_vm)
        set(const_target test_xir_exported_i64_const_${const_flow}_${const_mode})
        add_executable(${const_target} "${CMAKE_CURRENT_LIST_DIR}/test_xir_exported_i64_const_execution.c" "${const_c}")
        target_link_libraries(${const_target} PRIVATE xray_xir_vm)
        if(const_mode STREQUAL "native")
            target_compile_definitions(${const_target} PRIVATE CI_CONSUMER_KIND=1)
        elseif(const_mode STREQUAL "vm_native")
            target_compile_definitions(${const_target} PRIVATE CI_CONSUMER_KIND=4)
        else()
            target_compile_definitions(${const_target} PRIVATE CI_CONSUMER_KIND=2)
        endif()
        target_compile_definitions(${const_target} PRIVATE XR_EXPORTED_I64_CONST_PACKET="${const_packet}")
        list(APPEND XR_EXPORTED_I64_CONST_TARGETS ${const_target})
        add_test(NAME ${const_target} COMMAND ${const_target})
    endforeach()
    set_tests_properties(test_xir_exported_i64_const_${const_flow}_vm test_xir_exported_i64_const_${const_flow}_native
        test_xir_exported_i64_const_${const_flow}_vm_native test_xir_exported_i64_const_${const_flow}_native_vm
        PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE LABELS "unit;xir;const;library;ownership;native;mixed;abi")
endforeach()
foreach(const_target IN LISTS XR_EXPORTED_I64_CONST_TARGETS)
    set_target_properties(${const_target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${const_target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${const_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
set_tests_properties(test_xir_exported_i64_const_authority PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;const;library;ownership;authority")

add_test(NAME xir_public_const_cli
    COMMAND ${XRAY_PYTHON} -X utf8 ${CMAKE_CURRENT_LIST_DIR}/test_public_const_cli.py
        --binary $<TARGET_FILE:xray>
        --fixtures ${CMAKE_CURRENT_LIST_DIR}/public_const_inputs
        --output ${CMAKE_CURRENT_BINARY_DIR}/public-const-cli)
set_tests_properties(xir_public_const_cli PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE
    LABELS "unit;xir;source;const;library;ownership;native;cli")

add_executable(test_xir_public_slot_runtime "${CMAKE_CURRENT_LIST_DIR}/test_xir_public_slot_runtime.c")
target_link_libraries(test_xir_public_slot_runtime PRIVATE xray_xir_source xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_public_slot_runtime PRIVATE XR_EXPORTED_I64_CONST_WORK="${XR_EXPORTED_I64_CONST_WORK}")
set_target_properties(test_xir_public_slot_runtime PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_public_slot_runtime PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_public_slot_runtime PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME xir_public_slot_runtime COMMAND test_xir_public_slot_runtime)
set_tests_properties(xir_public_slot_runtime PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE
    LABELS "unit;xir;source;const;ownership;authority;runtime")
