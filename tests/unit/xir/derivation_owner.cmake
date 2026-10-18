add_executable(test_xir_derivation_owner "${CMAKE_CURRENT_LIST_DIR}/test_xir_derivation_owner.c")
target_link_libraries(test_xir_derivation_owner PRIVATE xray_xir_admission)
set_target_properties(test_xir_derivation_owner PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_derivation_owner PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_derivation_owner PRIVATE -Wall -Wextra -Werror)
endif()
foreach(grant_owner_case IN ITEMS ownership invalid budgets)
    add_test(NAME test_xir_derivation_owner_${grant_owner_case} COMMAND test_xir_derivation_owner ${grant_owner_case})
    set_tests_properties(test_xir_derivation_owner_${grant_owner_case} PROPERTIES TIMEOUT 30
        LABELS "unit;xir;derivation;ownership;receiving")
endforeach()

add_executable(test_xir_derivation_check "${CMAKE_CURRENT_LIST_DIR}/test_xir_derivation_check.c")
target_link_libraries(test_xir_derivation_check PRIVATE xray_xir xray_xir_cgen)
set_target_properties(test_xir_derivation_check PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_derivation_check PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_derivation_check PRIVATE -Wall -Wextra -Werror)
endif()
foreach(derivation_check_case IN ITEMS positive negative projection ordered_instance previous_schema constructor_policy)
    add_test(NAME test_xir_derivation_check_${derivation_check_case} COMMAND test_xir_derivation_check ${derivation_check_case})
    set_tests_properties(test_xir_derivation_check_${derivation_check_case} PROPERTIES TIMEOUT 60
        LABELS "unit;xir;derivation;authority;receiving")
endforeach()
add_test(NAME test_xir_derivation_independent_vectors
    COMMAND ${XRAY_PYTHON} ${CMAKE_CURRENT_LIST_DIR}/derive_derivation74_vectors.py
        --source-root ${PROJECT_SOURCE_DIR} --output-root ${PROJECT_SOURCE_DIR})
set_tests_properties(test_xir_derivation_independent_vectors PROPERTIES TIMEOUT 30 LABELS "unit;xir;derivation;wire;independent")

set(XIR_DERIVATION_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_derivation_authority.c)
add_custom_command(OUTPUT ${XIR_DERIVATION_GENERATED}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
    COMMAND $<TARGET_FILE:test_xir_derivation_check> --emit-native ${XIR_DERIVATION_GENERATED}
    DEPENDS test_xir_derivation_check
    VERBATIM)
add_executable(test_xir_derivation_native "${CMAKE_CURRENT_LIST_DIR}/test_xir_derivation_native.c" ${XIR_DERIVATION_GENERATED})
target_link_libraries(test_xir_derivation_native PRIVATE xray_xir_scalar)
set_target_properties(test_xir_derivation_native PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_derivation_native PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_derivation_native PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_xir_derivation_native COMMAND test_xir_derivation_native)
set_tests_properties(test_xir_derivation_native PROPERTIES TIMEOUT 60 LABELS "unit;xir;derivation;authority;execution;receiving;abi")
