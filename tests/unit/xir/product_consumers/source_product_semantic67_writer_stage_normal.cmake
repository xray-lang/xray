# Observe real public writer charges under the existing finite compiler owner.
set(writer_stage_normal_target test_source_product_semantic67_writer_stage_normal)
add_executable(${writer_stage_normal_target}
    "${CMAKE_CURRENT_LIST_DIR}/semantic67_named/test_source_product_semantic67_writer_stage_normal.c")
target_link_libraries(${writer_stage_normal_target} PRIVATE xray_xir_source_product)
target_include_directories(${writer_stage_normal_target} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/..")
set_target_properties(${writer_stage_normal_target} PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(${writer_stage_normal_target} PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(${writer_stage_normal_target} PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME ${writer_stage_normal_target} COMMAND ${writer_stage_normal_target})
set_tests_properties(${writer_stage_normal_target} PROPERTIES TIMEOUT 120
    LABELS "unit;xir;program-consumer;ownership;checked;public-writer;normal-resources")

# The owner freezes this configuration's normal N before registering the replay.
function(xray_register_writer_packet_oom normal_sites)
    if(NOT "${normal_sites}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "Public writer OOM requires the frozen cold normal allocation count")
    endif()
    add_test(NAME test_source_product_semantic67_writer_packet_oom
        COMMAND test_source_product_semantic67_writer_stage_normal --packet-oom-last "${normal_sites}")
    set_tests_properties(test_source_product_semantic67_writer_packet_oom PROPERTIES
        TIMEOUT 120 LABELS "unit;xir;program-consumer;ownership;checked;public-writer;fault-injection")
endfunction()

xray_register_writer_packet_oom(10)
