# Root integrates this registration and the generated provider output.
set(XIR_PRELUDE_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_prelude_source.xrc)
set(XIR_PRELUDE_CATALOG ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_prelude_catalog.xrc)
add_library(xir_prelude_source_runtime OBJECT xir/test_xir_prelude_execution.c)
target_compile_definitions(xir_prelude_source_runtime PRIVATE CONSUMER_KIND=3)
target_link_libraries(xir_prelude_source_runtime PRIVATE xray_xir_vm)
add_executable(test_xir_prelude_source xir/test_xir_prelude_source.c $<TARGET_OBJECTS:xir_prelude_source_runtime>)
target_link_libraries(test_xir_prelude_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_prelude_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_prelude"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
file(GLOB XIR_PRELUDE_FIXTURES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_prelude/*.xr")
add_custom_command(OUTPUT ${XIR_PRELUDE_SOURCE} ${XIR_PRELUDE_CATALOG}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_prelude_source> ${XIR_PRELUDE_SOURCE} ${XIR_PRELUDE_CATALOG}
    DEPENDS test_xir_prelude_source ${XIR_PRELUDE_FIXTURES}
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_prelude_source_cases.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_prelude_runtime.h
    VERBATIM)
add_executable(test_xir_prelude_packet xir/test_xir_prelude_execution.c)
target_compile_definitions(test_xir_prelude_packet PRIVATE CONSUMER_KIND=0)
target_link_libraries(test_xir_prelude_packet PRIVATE xray_xir_vm xray_xir_cgen)
set(XIR_PRELUDE_TARGETS xir_prelude_source_runtime test_xir_prelude_source test_xir_prelude_packet)
foreach(prelude_flow IN ITEMS source catalog)
    if(prelude_flow STREQUAL "source")
        set(prelude_checked ${XIR_PRELUDE_SOURCE})
    else()
        set(prelude_checked ${XIR_PRELUDE_CATALOG})
    endif()
    set(prelude_generated ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_prelude_${prelude_flow}.c)
    add_custom_command(OUTPUT ${prelude_generated}
        COMMAND $<TARGET_FILE:test_xir_prelude_packet> ${prelude_checked} ${prelude_generated}
        DEPENDS test_xir_prelude_packet ${prelude_checked} VERBATIM)
    add_test(NAME test_xir_prelude_${prelude_flow}_vm COMMAND test_xir_prelude_packet
        ${prelude_checked} ${CMAKE_CURRENT_BINARY_DIR}/xir_prelude_${prelude_flow}_vm_test.c)
    foreach(prelude_mode IN ITEMS native vm_native native_vm)
        set(prelude_target test_xir_prelude_${prelude_flow}_${prelude_mode})
        add_executable(${prelude_target} xir/test_xir_prelude_execution.c ${prelude_generated})
        target_link_libraries(${prelude_target} PRIVATE xray_xir_vm)
        if(prelude_mode STREQUAL "native")
            target_compile_definitions(${prelude_target} PRIVATE CONSUMER_KIND=1)
        elseif(prelude_mode STREQUAL "vm_native")
            target_compile_definitions(${prelude_target} PRIVATE CONSUMER_KIND=2)
        else()
            target_compile_definitions(${prelude_target} PRIVATE CONSUMER_KIND=4)
        endif()
        list(APPEND XIR_PRELUDE_TARGETS ${prelude_target})
        add_test(NAME ${prelude_target} COMMAND ${prelude_target})
    endforeach()
    set_tests_properties(test_xir_prelude_${prelude_flow}_vm test_xir_prelude_${prelude_flow}_native
        test_xir_prelude_${prelude_flow}_vm_native test_xir_prelude_${prelude_flow}_native_vm
        PROPERTIES LABELS "unit;xir;execution;ownership;abi;budget" TIMEOUT 600 RUN_SERIAL TRUE)
endforeach()
foreach(prelude_target IN LISTS XIR_PRELUDE_TARGETS)
    if(MSVC)
        target_compile_options(${prelude_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${prelude_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_xir_prelude_source COMMAND test_xir_prelude_source)
set_tests_properties(test_xir_prelude_source PROPERTIES
    LABELS "unit;xir;execution;ownership;abi;budget" TIMEOUT 1200 RUN_SERIAL TRUE COST 120)
