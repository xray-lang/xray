



foreach(fact IN ITEMS function generic type stateful cell cell-write array array-const class class-array class-ref class-inline-ref active-birth)
    string(REPLACE "-" "_" suffix "${fact}")
    if(fact STREQUAL "active-birth")
        set(suffix "${fact}")
    endif()

endforeach()

foreach(kind IN ITEMS definitions definitions-comptime definitions-import definitions-cell)
    string(REPLACE "-" "_" suffix "${kind}")

endforeach()




set(history_native_dir "${CMAKE_CURRENT_BINARY_DIR}/source_history_native")
set(history_native_header "${history_native_dir}/source_history_native_generated.h")
set(history_native_sources)
foreach(history_mode IN ITEMS read write array array_const function_call function_value class class_array class_ref class_inline_ref)
    foreach(history_generation RANGE 1 4)
        list(APPEND history_native_sources "${history_native_dir}/source_history_${history_mode}_${history_generation}.c")
    endforeach()
endforeach()



foreach(history_backend IN ITEMS native vm_native native_vm)
    set(history_target test_xir_p10_source_history_${history_backend})



    foreach(history_kind IN ITEMS cell cell-write array array-const function-call function-value class class-array class-ref class-inline-ref)
        string(REPLACE "-" "_" history_suffix "${history_kind}")

    endforeach()
endforeach()

add_executable(test_xir_p10_initializer_surface "${CMAKE_CURRENT_LIST_DIR}/test_initializer_surface.c")
add_executable(test_xir_p10_type_surface "${CMAKE_CURRENT_LIST_DIR}/test_type_surface.c")
target_link_libraries(test_xir_p10_type_surface PRIVATE xray_xir_source xray_frontend_format)
set_target_properties(test_xir_p10_type_surface PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p10_type_surface PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_p10_type_surface PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p10_type_surface COMMAND test_xir_p10_type_surface)
set_tests_properties(test_xir_p10_type_surface PROPERTIES TIMEOUT 30 LABELS "unit;xir;p10;parser;ownership;const")

target_link_libraries(test_xir_p10_initializer_surface PRIVATE xray_xir_source xray_frontend_format)
set_target_properties(test_xir_p10_initializer_surface PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_p10_initializer_surface PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(test_xir_p10_initializer_surface PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_p10_initializer_surface COMMAND test_xir_p10_initializer_surface)
set_tests_properties(test_xir_p10_initializer_surface PROPERTIES TIMEOUT 30 LABELS "unit;xir;p10;parser;init;ownership")
















































































set(reference_native_targets)
set(native_generic_class_phantom "${CMAKE_CURRENT_BINARY_DIR}/generated_native/generic_class_phantom.c")


set(native_explicit_witness "${CMAKE_CURRENT_BINARY_DIR}/generated_native/explicit_witness.c")




list(APPEND reference_native_targets )





list(APPEND reference_native_targets )


set(native_witness_forward "${CMAKE_CURRENT_BINARY_DIR}/generated_native/witness_forward.c")




list(APPEND reference_native_targets )





list(APPEND reference_native_targets )


set(native_witness_diamond "${CMAKE_CURRENT_BINARY_DIR}/generated_native/witness_diamond.c")




list(APPEND reference_native_targets )





list(APPEND reference_native_targets )


set(native_generic_method_value "${CMAKE_CURRENT_BINARY_DIR}/generated_native/generic_method_value.c")




list(APPEND reference_native_targets )





list(APPEND reference_native_targets )


set(native_optional_layers "${CMAKE_CURRENT_BINARY_DIR}/generated_native/optional_layers.c")




list(APPEND reference_native_targets )





list(APPEND reference_native_targets )


set(native_optional_lazy_absent "${CMAKE_CURRENT_BINARY_DIR}/generated_native/optional_lazy_absent.c")




list(APPEND reference_native_targets )





list(APPEND reference_native_targets )


set(native_optional_lazy_inner "${CMAKE_CURRENT_BINARY_DIR}/generated_native/optional_lazy_inner.c")




list(APPEND reference_native_targets )





list(APPEND reference_native_targets )


set(native_optional_exact_context "${CMAKE_CURRENT_BINARY_DIR}/generated_native/optional_exact_context.c")




list(APPEND reference_native_targets )





list(APPEND reference_native_targets )


set(native_optional_triple_claims "${CMAKE_CURRENT_BINARY_DIR}/generated_native/optional_triple_claims.c")




list(APPEND reference_native_targets )





list(APPEND reference_native_targets )


set(native_for_and_ternary "${CMAKE_CURRENT_BINARY_DIR}/generated_native/for_and_ternary.c")




list(APPEND reference_native_targets )





set(native_const_values "${CMAKE_CURRENT_BINARY_DIR}/generated_native/const_values.c")






add_custom_target(xir_p10_native_references DEPENDS ${reference_native_targets})

xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
include("${CMAKE_CURRENT_LIST_DIR}/reception_patterns.cmake")

include("${CMAKE_CURRENT_LIST_DIR}/string_members.cmake")

foreach(storage_kind IN ITEMS array array-const)
    string(REPLACE "-" "_" storage_suffix "${storage_kind}")

endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/string_builder_source.cmake")

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/library_inventory")
