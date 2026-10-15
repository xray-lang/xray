# Generate the authentic Source inputs once; rejected inputs emit no C.
set(CLASS_ACCESS_NATIVE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/class_access_native")
set(CLASS_ACCESS_NATIVE_C)
foreach(fixture IN ITEMS 0 1 3 5)
    list(APPEND CLASS_ACCESS_NATIVE_C "${CLASS_ACCESS_NATIVE_DIR}/class_access_${fixture}.c")
endforeach()
set(CLASS_ACCESS_NATIVE_H "${CLASS_ACCESS_NATIVE_DIR}/class_access_generated.h")
set(CLASS_ACCESS_NATIVE_INPUTS)
foreach(fixture IN ITEMS worker_return_formal worker_array_cow worker_carrier_unknown same_owner worker_module_alias worker_ref_array)
    list(APPEND CLASS_ACCESS_NATIVE_INPUTS "${CMAKE_SOURCE_DIR}/tests/fixtures/xir_cell_owner/class_access_${fixture}.xr")
endforeach()
add_custom_command(OUTPUT ${CLASS_ACCESS_NATIVE_C} "${CLASS_ACCESS_NATIVE_H}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CLASS_ACCESS_NATIVE_DIR}"
    COMMAND $<TARGET_FILE:test_xir_class_access_source_resources> --write-c
        ${CLASS_ACCESS_NATIVE_C} "${CLASS_ACCESS_NATIVE_H}"
    DEPENDS test_xir_class_access_source_resources ${CLASS_ACCESS_NATIVE_INPUTS}
        "${CMAKE_CURRENT_LIST_DIR}/xir_class_access_native_producer.h"
        "${CMAKE_CURRENT_LIST_DIR}/xir_cell_owner_source_pipeline.h"
    VERBATIM)
foreach(backend IN ITEMS native mixed)
    set(target test_xir_class_access_${backend})
    add_executable(${target} xir/test_xir_class_access_native.c ${CLASS_ACCESS_NATIVE_C} "${CLASS_ACCESS_NATIVE_H}")
    target_link_libraries(${target} PRIVATE xray_xir_source xray_xir_vm)
    target_include_directories(${target} PRIVATE "${CLASS_ACCESS_NATIVE_DIR}")
    target_compile_definitions(${target} PRIVATE
        XR_CELL_OWNER_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures/xir_cell_owner")
    if(backend STREQUAL "native")
        target_compile_definitions(${target} PRIVATE XR_CLASS_ACCESS_BACKEND=1)
    else()
        target_compile_definitions(${target} PRIVATE XR_CLASS_ACCESS_BACKEND=2)
    endif()
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
    foreach(mode IN ITEMS normal runtime-fi runtime-axes runtime-prepare runtime-control)
        string(REPLACE "-" "_" suffix "${mode}")
        add_test(NAME test_xir_class_access_${backend}_${suffix} COMMAND ${target} --${mode})
        set_tests_properties(test_xir_class_access_${backend}_${suffix} PROPERTIES
            LABELS "unit;xir;ownership;source;class;generated-c;${backend};resources"
            TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE)
    endforeach()
endforeach()
