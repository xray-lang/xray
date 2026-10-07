# Original complete default assertion fixtures; separate cold process per input.
set(source_default_assertion_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_default_assertion_assertion_owner_cleanup "${CMAKE_BINARY_DIR}/generated/source-default-assertion-group2/assertion_owner_cleanup")
file(MAKE_DIRECTORY "${source_default_assertion_assertion_owner_cleanup}")
file(GENERATE OUTPUT "${source_default_assertion_assertion_owner_cleanup}/root.xr" CONTENT [==[class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
assert(ok)
return first.value + second.value
}
fn answer() -> i64 { return checked(false) }
@test
fn checkAssertionFailure() { answer() }
@test
fn checkAssertionSuccess() { assert(checked(true) == 42) }
]==] NEWLINE_STYLE LF)
set(source_default_assertion_assertion_defer "${CMAKE_BINARY_DIR}/generated/source-default-assertion-group2/assertion_defer")
file(MAKE_DIRECTORY "${source_default_assertion_assertion_defer}")
file(GENERATE OUTPUT "${source_default_assertion_assertion_defer}/root.xr" CONTENT [==[var cleanupCount: i64 = 0
fn readCleanupCount() -> i64 { return cleanupCount }
class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
defer { if (ok) { cleanupCount = 11 } else { cleanupCount = 22 } }
assert(ok)
return first.value + second.value
}
fn answer() -> i64 { return checked(false) + readCleanupCount() }
@test
fn checkFreshCleanup() { assert(readCleanupCount() == 0) }
@test
fn checkAssertionFailure() { answer() }
@test
fn checkFailureCleanup() { assert(readCleanupCount() == 22) }
@test
fn checkAssertionSuccess() { assert(checked(true) == 42); assert(readCleanupCount() == 11) }
]==] NEWLINE_STYLE LF)
add_executable(test_source_product_default_assertion_group2 "${source_default_assertion_dir}/test_source_product_default_assertion_group2.c")
target_link_libraries(test_source_product_default_assertion_group2 PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_default_assertion_group2 PRIVATE "${source_default_assertion_dir}/..")
set_target_properties(test_source_product_default_assertion_group2 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_default_assertion_group2 PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_default_assertion_group2 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_default_assertion_group2 PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_default_assertion_group2_assertion_owner_cleanup_vm_normal COMMAND test_source_product_default_assertion_group2 assertion_owner_cleanup
    "${source_default_assertion_assertion_owner_cleanup}" "${source_default_assertion_assertion_owner_cleanup}/root.xr")
set_tests_properties(test_source_product_default_assertion_group2_assertion_owner_cleanup_vm_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;assertion;ownership;vm-projection")
add_test(NAME test_source_product_default_assertion_group2_assertion_defer_vm_normal COMMAND test_source_product_default_assertion_group2 assertion_defer
    "${source_default_assertion_assertion_defer}" "${source_default_assertion_assertion_defer}/root.xr")
set_tests_properties(test_source_product_default_assertion_group2_assertion_defer_vm_normal PROPERTIES TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;assertion;ownership;vm-projection")
