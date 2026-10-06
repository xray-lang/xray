# Source-free roles retain full current and retired packet bytes.
set(packet_roles_source "${CMAKE_CURRENT_LIST_DIR}/test_source_product_packet_roles.c")
set(packet_roles_vm test_source_product_packet_roles)
set(packet_roles_native test_source_product_packet_roles_native)
set(packet_roles_c "${CMAKE_BINARY_DIR}/generated/source_product_packet_roles.c")
add_executable(${packet_roles_vm} "${packet_roles_source}")
add_custom_command(OUTPUT "${packet_roles_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:${packet_roles_vm}> --emit-c "${packet_roles_c}"
    DEPENDS ${packet_roles_vm} VERBATIM)
add_executable(${packet_roles_native} "${packet_roles_source}" "${packet_roles_c}")
target_compile_definitions(${packet_roles_native} PRIVATE XR_PACKET_ROLES_NATIVE=1)
foreach(target IN ITEMS ${packet_roles_vm} ${packet_roles_native})
    target_link_libraries(${target} PRIVATE xray_xir_source_product)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_packet_roles_vm COMMAND ${packet_roles_vm})
add_test(NAME test_source_product_packet_roles_native_and_mixed COMMAND ${packet_roles_native})
set_tests_properties(test_source_product_packet_roles_vm test_source_product_packet_roles_native_and_mixed
    PROPERTIES TIMEOUT 120 LABELS "unit;xir;program-consumer;checked;ownership;execution")
