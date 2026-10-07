# Complete original two-module nominal catch through generated native callbacks.
set(imported_enum_native_dir "${CMAKE_CURRENT_LIST_DIR}")
set(imported_enum_native_root "${CMAKE_BINARY_DIR}/generated/source-imported-enum-native")
set(imported_enum_native_c "${imported_enum_native_root}/program.c")
file(MAKE_DIRECTORY "${imported_enum_native_root}")
file(GENERATE OUTPUT "${imported_enum_native_root}/main.xr" CONTENT [==[import { Problem as ImportedProblem, fail } from "./library"
fn localFail() -> i64 { throw ImportedProblem.Failed }
fn answer() -> i64 {
 var result = 0
 try { result = fail() } catch (e: ImportedProblem) { result = 20 }
 try { result = localFail() } catch (e: ImportedProblem) { result = result + 22 }
 assert(result == 42)
 return result
}
@test
fn checkImportedIdentity() { assert(answer() == 42) }
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${imported_enum_native_root}/library.xr" CONTENT [==[export enum Problem { Failed }
export fn fail() -> i64 { throw Problem.Failed }
]==] NEWLINE_STYLE LF)
add_executable(test_source_product_imported_enum_catch_c_emitter
    "${imported_enum_native_dir}/test_source_product_imported_enum_catch_native.c")

target_link_libraries(test_source_product_imported_enum_catch_c_emitter PRIVATE xray_xir_source_product)

target_include_directories(test_source_product_imported_enum_catch_c_emitter PRIVATE "${imported_enum_native_dir}/..")
add_custom_command(OUTPUT "${imported_enum_native_c}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${imported_enum_native_root}"
    COMMAND $<TARGET_FILE:test_source_product_imported_enum_catch_c_emitter>
        "${imported_enum_native_root}" "${imported_enum_native_root}/main.xr" "${imported_enum_native_root}/library.xr"
        --emit "${imported_enum_native_c}"
    DEPENDS test_source_product_imported_enum_catch_c_emitter
        "${imported_enum_native_root}/main.xr" "${imported_enum_native_root}/library.xr"
    VERBATIM)
add_executable(test_source_product_imported_enum_catch_native
    "${imported_enum_native_dir}/test_source_product_imported_enum_catch_native.c" "${imported_enum_native_c}")

target_compile_definitions(test_source_product_imported_enum_catch_native PRIVATE XR_SOURCE_IMPORTED_ENUM_NATIVE=1)

target_link_libraries(test_source_product_imported_enum_catch_native PRIVATE xray_xir_source_product)

target_include_directories(test_source_product_imported_enum_catch_native PRIVATE "${imported_enum_native_dir}/..")
foreach(target IN ITEMS test_source_product_imported_enum_catch_c_emitter test_source_product_imported_enum_catch_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
add_test(NAME test_source_product_imported_enum_catch_native_normal COMMAND test_source_product_imported_enum_catch_native
    "${imported_enum_native_root}" "${imported_enum_native_root}/main.xr" "${imported_enum_native_root}/library.xr")
set_tests_properties(test_source_product_imported_enum_catch_native_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;typed-error;ownership;native-projection")
