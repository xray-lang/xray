# Complete original Array sources keep separate generated payloads and strict panic expectations.
set(original_array_cleanup_dir "${CMAKE_CURRENT_LIST_DIR}")
set(original_array_cleanup_source "${original_array_cleanup_dir}/test_original_array_cleanup_consumer.c")
set(original_array_cleanup_glue "${original_array_cleanup_dir}/original_array_cleanup_native.inc.c")
set(original_array_cleanup_generated "${CMAKE_BINARY_DIR}/generated/original-array-cleanup")
set(original_array_cleanup_stdlib "${CMAKE_SOURCE_DIR}/stdlib")
add_executable(test_xir_original_array_cleanup_consumer "${original_array_cleanup_source}")
target_link_libraries(test_xir_original_array_cleanup_consumer PRIVATE xray_xir_source_product)
target_include_directories(test_xir_original_array_cleanup_consumer PRIVATE
    "${original_array_cleanup_dir}" "${original_array_cleanup_dir}/..")
set(original_array_cleanup_targets test_xir_original_array_cleanup_consumer)

foreach(original_array_cleanup_kind IN ITEMS default invoke)
    set(original_array_cleanup_family "array_${original_array_cleanup_kind}_defer")
    set(original_array_cleanup_fixture "${original_array_cleanup_dir}/original_${original_array_cleanup_family}_root.xr")
    set(original_array_cleanup_native "test_xir_original_${original_array_cleanup_family}_native_consumer")
    set(original_array_cleanup_c "${original_array_cleanup_generated}/${original_array_cleanup_family}.c")
    set(original_array_cleanup_partial "${original_array_cleanup_c}.partial")
    add_custom_command(OUTPUT "${original_array_cleanup_c}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${original_array_cleanup_generated}"
        COMMAND $<TARGET_FILE:test_xir_original_array_cleanup_consumer>
            "${original_array_cleanup_family}" 0 "${original_array_cleanup_dir}"
            "${original_array_cleanup_fixture}" "${original_array_cleanup_stdlib}"
            --emit "${original_array_cleanup_partial}"
        COMMAND ${CMAKE_COMMAND} -E rename "${original_array_cleanup_partial}" "${original_array_cleanup_c}"
        DEPENDS test_xir_original_array_cleanup_consumer "${original_array_cleanup_fixture}"
            "${original_array_cleanup_source}" "${original_array_cleanup_glue}"
            "${original_array_cleanup_dir}/../xir_instance_compile_observer.h"
            "${original_array_cleanup_dir}/../xir_runtime_allocations.h"
        VERBATIM)
    add_executable(${original_array_cleanup_native}
        "${original_array_cleanup_source}" "${original_array_cleanup_c}")
    target_compile_definitions(${original_array_cleanup_native} PRIVATE ARRAY_CLEANUP_NATIVE=1)
    target_link_libraries(${original_array_cleanup_native} PRIVATE xray_xir_source_product)
    target_include_directories(${original_array_cleanup_native} PRIVATE
        "${original_array_cleanup_dir}" "${original_array_cleanup_dir}/..")
    list(APPEND original_array_cleanup_targets ${original_array_cleanup_native})

    set(original_array_cleanup_test_prefix "test_original_${original_array_cleanup_family}")
    add_test(NAME ${original_array_cleanup_test_prefix}_vm
        COMMAND test_xir_original_array_cleanup_consumer "${original_array_cleanup_family}" 0
            "${original_array_cleanup_dir}" "${original_array_cleanup_fixture}" "${original_array_cleanup_stdlib}")
    add_test(NAME ${original_array_cleanup_test_prefix}_native
        COMMAND ${original_array_cleanup_native} "${original_array_cleanup_family}" 1
            "${original_array_cleanup_dir}" "${original_array_cleanup_fixture}" "${original_array_cleanup_stdlib}")
    add_test(NAME ${original_array_cleanup_test_prefix}_mixed_even
        COMMAND ${original_array_cleanup_native} "${original_array_cleanup_family}" 2
            "${original_array_cleanup_dir}" "${original_array_cleanup_fixture}" "${original_array_cleanup_stdlib}")
    add_test(NAME ${original_array_cleanup_test_prefix}_mixed_odd
        COMMAND ${original_array_cleanup_native} "${original_array_cleanup_family}" 3
            "${original_array_cleanup_dir}" "${original_array_cleanup_fixture}" "${original_array_cleanup_stdlib}")
    set_tests_properties(${original_array_cleanup_test_prefix}_vm ${original_array_cleanup_test_prefix}_native
        ${original_array_cleanup_test_prefix}_mixed_even ${original_array_cleanup_test_prefix}_mixed_odd
        PROPERTIES TIMEOUT 120 PROCESSORS 1
        LABELS "unit;xir;source-product;program-consumer;array;panic;defer;ownership;original-source")
endforeach()

foreach(original_array_cleanup_target IN LISTS original_array_cleanup_targets)
    set_target_properties(${original_array_cleanup_target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${original_array_cleanup_target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${original_array_cleanup_target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${original_array_cleanup_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
