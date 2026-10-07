# Complete original imported enum catch; independent finite VM normal process.
set(source_imported_enum_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_imported_enum_root "${CMAKE_BINARY_DIR}/generated/source-imported-enum-catch")
file(MAKE_DIRECTORY "${source_imported_enum_root}")
file(GENERATE OUTPUT "${source_imported_enum_root}/main.xr" CONTENT [==[import { Problem as ImportedProblem, fail } from "./library"
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
file(GENERATE OUTPUT "${source_imported_enum_root}/library.xr" CONTENT [==[export enum Problem { Failed }
export fn fail() -> i64 { throw Problem.Failed }
]==] NEWLINE_STYLE LF)
add_executable(test_source_product_imported_enum_catch "${source_imported_enum_dir}/test_source_product_imported_enum_catch.c")
target_link_libraries(test_source_product_imported_enum_catch PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_imported_enum_catch PRIVATE "${source_imported_enum_dir}/..")
set_target_properties(test_source_product_imported_enum_catch PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_imported_enum_catch PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_imported_enum_catch PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_imported_enum_catch PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_imported_enum_catch_vm_normal COMMAND test_source_product_imported_enum_catch
    "${source_imported_enum_root}" "${source_imported_enum_root}/main.xr" "${source_imported_enum_root}/library.xr")
set_tests_properties(test_source_product_imported_enum_catch_vm_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;typed-error;ownership;vm-projection")
