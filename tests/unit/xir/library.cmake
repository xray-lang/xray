# One entryless Library produces the sole linked Checked program and generated C.
set(XIR_LIBRARY_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library.xrc)
set(XIR_LIBRARY_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_library.c)
set(XIR_LIBRARY_SOURCE_WORK ${CMAKE_CURRENT_BINARY_DIR}/library-source-work)
file(MAKE_DIRECTORY ${XIR_LIBRARY_SOURCE_WORK})
add_executable(test_xir_library_source xir/test_xir_library_source.c)
target_sources(test_xir_library_source PRIVATE xir/xir_library_source_runtime.c)
target_link_libraries(test_xir_library_source PRIVATE xray_xir_source xray_xir_vm)
target_compile_definitions(test_xir_library_source PRIVATE
    XR_SOURCE_FIXTURES="${XIR_LIBRARY_SOURCE_WORK}"
    XR_SOURCE_STDLIB="${CMAKE_SOURCE_DIR}/stdlib")
add_custom_command(OUTPUT ${XIR_LIBRARY_CHECKED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_library_source> ${XIR_LIBRARY_CHECKED}
    DEPENDS test_xir_library_source
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_goldens.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_defaults_golden_bytes.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_defaults_wire_cases.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_defaults_invoke_golden.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_map_cases.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_reader_cases.h
        ${CMAKE_CURRENT_SOURCE_DIR}/xir/xir_library_dispatch_cases.h
    VERBATIM)
add_executable(test_xir_library_packet xir/test_xir_library_execution.c ${XIR_LIBRARY_CHECKED})
target_compile_definitions(test_xir_library_packet PRIVATE CONSUMER_KIND=0 XR_CHECKED_FIXTURE="${XIR_LIBRARY_CHECKED}")
target_link_libraries(test_xir_library_packet PRIVATE xray_xir_vm xray_xir_cgen)
add_custom_command(OUTPUT ${XIR_LIBRARY_GENERATED}
    COMMAND $<TARGET_FILE:test_xir_library_packet> ${XIR_LIBRARY_GENERATED}
    DEPENDS test_xir_library_packet ${XIR_LIBRARY_CHECKED}
    VERBATIM)
foreach(library_mode IN ITEMS native mixed)
    add_executable(test_xir_library_${library_mode} xir/test_xir_library_execution.c ${XIR_LIBRARY_GENERATED})
    target_link_libraries(test_xir_library_${library_mode} PRIVATE xray_xir_vm)
    if(library_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_library_${library_mode} PRIVATE CONSUMER_KIND=2)
    else()
        target_compile_definitions(test_xir_library_${library_mode} PRIVATE CONSUMER_KIND=1)
    endif()
endforeach()
foreach(library_test IN ITEMS test_xir_library_source test_xir_library_packet test_xir_library_native test_xir_library_mixed)
    if(MSVC)
        target_compile_options(${library_test} PRIVATE /W4 /WX)
    else()
        target_compile_options(${library_test} PRIVATE -Wall -Wextra -Werror)
    endif()
    add_test(NAME ${library_test} COMMAND ${library_test})
    set_tests_properties(${library_test} PROPERTIES LABELS "unit;xir;execution;ownership;abi")
endforeach()

add_test(NAME test_xir_library_atomic64_vectors COMMAND ${XRAY_PYTHON}
    ${CMAKE_CURRENT_SOURCE_DIR}/xir/derive_library_atomic64_vectors.py)
set_tests_properties(test_xir_library_atomic64_vectors PROPERTIES LABELS "unit;xir;abi")
