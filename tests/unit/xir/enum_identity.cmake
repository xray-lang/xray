# Dedicated enum identity fixture stays below the existing packet budget.
set(XIR_ENUM_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_enum_identity.xrc)
set(XIR_ENUM_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_enum_identity.c)
add_xray_bootstrap_executable(test_xir_enum_source xir/test_xir_enum_source.c)
target_link_libraries(test_xir_enum_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_enum_source PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_enum_identity"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_ENUM_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_enum_source> ${XIR_ENUM_CHECKED}
    DEPENDS test_xir_enum_source
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_enum_identity/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_enum_identity/enum_identity_decl.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_enum_identity/enum_identity_value.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_enum_identity/enum_identity_reader.xr
    VERBATIM)
add_executable(test_xir_enum_packet xir/test_xir_enum_packet.c ${XIR_ENUM_CHECKED})
target_link_libraries(test_xir_enum_packet PRIVATE xray_xir_vm xray_xir_cgen)
target_compile_definitions(test_xir_enum_packet PRIVATE XR_CHECKED_FIXTURE="${XIR_ENUM_CHECKED}")
add_custom_command(OUTPUT ${XIR_ENUM_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_enum_packet> ${XIR_ENUM_GENERATED}
    DEPENDS test_xir_enum_packet ${XIR_ENUM_CHECKED}
    VERBATIM)
add_executable(test_xir_enum_native xir/test_xir_enum_native.c ${XIR_ENUM_GENERATED})
target_link_libraries(test_xir_enum_native PRIVATE xray_xir_scalar)
add_xray_bootstrap_executable(test_xir_enum_mixed xir/test_xir_enum_mixed.c ${XIR_ENUM_GENERATED})
target_link_libraries(test_xir_enum_mixed PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_enum_mixed PRIVATE
    XR_SOURCE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_enum_identity"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
foreach(enum_test IN ITEMS test_xir_enum_source test_xir_enum_packet test_xir_enum_native test_xir_enum_mixed)
    if(MSVC)
        target_compile_options(${enum_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${enum_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${enum_test} COMMAND ${enum_test})
    set_tests_properties(${enum_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi")
endforeach()
