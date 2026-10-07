# Complete original typed-division source executes only through private same-module test roles.
set(source_division_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_division_fixture "${CMAKE_BINARY_DIR}/generated/source-integer-division-defer")
file(MAKE_DIRECTORY "${source_division_fixture}")
file(GENERATE OUTPUT "${source_division_fixture}/root.xr" CONTENT [==[var cleanupCount: i64 = 0
fn readCleanupCount() -> i64 { return cleanupCount }
class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
defer { if (ok) { cleanupCount = 11 } else { cleanupCount = 22 } }
var divisor: u8 = 0
if (ok) { divisor = 2 }
var result: u8 = 84 / divisor
return first.value + second.value + (result as i64) - 42
}
fn answer() -> i64 { return checked(false) + readCleanupCount() }
@test
fn checkFreshCleanup() { assert(readCleanupCount() == 0) }
@test
fn checkDivisionFailure() { answer() }
@test
fn checkFailureCleanup() { assert(readCleanupCount() == 22) }
@test
fn checkSuccessCleanup() { assert(checked(true) == 42); assert(readCleanupCount() == 11) }
]==] NEWLINE_STYLE LF)
add_executable(test_source_product_integer_division_defer
    "${source_division_dir}/test_source_product_integer_division_defer.c")
target_link_libraries(test_source_product_integer_division_defer PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_integer_division_defer PRIVATE "${source_division_dir}/..")
set_target_properties(test_source_product_integer_division_defer PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_integer_division_defer PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_integer_division_defer PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_integer_division_defer PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_integer_division_defer_vm_normal COMMAND test_source_product_integer_division_defer
    "${source_division_fixture}" "${source_division_fixture}/root.xr")
set_tests_properties(test_source_product_integer_division_defer_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;numeric-panic;ownership;vm-projection")
