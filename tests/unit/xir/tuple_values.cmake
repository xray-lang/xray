add_executable(test_xir_tuple_values xir/test_xir_tuple_values.c)
target_link_libraries(test_xir_tuple_values PRIVATE xray_xir_scalar xray_xir)
if(MSVC)
    target_compile_options(test_xir_tuple_values PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_tuple_values PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_tuple_values COMMAND test_xir_tuple_values)
set_tests_properties(test_xir_tuple_values PROPERTIES LABELS "unit;xir;ownership;tuple;runtime" TIMEOUT 180)
add_executable(test_xir_tuple_source xir/test_xir_tuple_source.c)
target_link_libraries(test_xir_tuple_source PRIVATE xray_xir_source_product)
target_compile_definitions(test_xir_tuple_source PRIVATE
    XR_TUPLE_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_tuple/runtime"
    XR_TUPLE_CLOSED_ROOT="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_tuple/closed"
    XR_TUPLE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
if(MSVC)
    target_compile_options(test_xir_tuple_source PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_tuple_source PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_tuple_source COMMAND test_xir_tuple_source)
set_tests_properties(test_xir_tuple_source PROPERTIES LABELS "unit;xir;ownership;tuple;source;execution" TIMEOUT 180)
set(XIR_TUPLE_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_tuple_source.c)
add_custom_command(OUTPUT ${XIR_TUPLE_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_tuple_source> ${XIR_TUPLE_GENERATED}
    DEPENDS test_xir_tuple_source ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_tuple/runtime/root.xr
        ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_tuple/runtime/library.xr
    VERBATIM)
add_executable(test_xir_tuple_native xir/test_xir_tuple_native.c ${XIR_TUPLE_GENERATED})
target_link_libraries(test_xir_tuple_native PRIVATE xray_xir_scalar)
if(MSVC)
    target_compile_options(test_xir_tuple_native PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_tuple_native PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_tuple_native COMMAND test_xir_tuple_native)
set_tests_properties(test_xir_tuple_native PROPERTIES LABELS "unit;xir;ownership;tuple;native;execution" TIMEOUT 180)
foreach(tuple_closed_case unit_parameter unit_generic equal spread index set bounds)
    add_test(NAME test_xir_tuple_closed_${tuple_closed_case} COMMAND test_xir_tuple_source --reject ${tuple_closed_case})
    set_tests_properties(test_xir_tuple_closed_${tuple_closed_case}
        PROPERTIES LABELS "unit;xir;ownership;tuple;source;rejection" TIMEOUT 180)
endforeach()
add_test(NAME test_xir_tuple_closed_destructure COMMAND test_xir_tuple_source --accept-destructure)
set_tests_properties(test_xir_tuple_closed_destructure
    PROPERTIES LABELS "unit;xir;ownership;tuple;source;execution" TIMEOUT 180)

include(${CMAKE_CURRENT_SOURCE_DIR}/xir/tuple_destructure.cmake)
