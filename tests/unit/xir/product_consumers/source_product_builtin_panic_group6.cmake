# Six complete original inputs; one cold process per fixture, same-module test authority only.
set(source_panic_group6_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_panic_group6_integer_remainder_zero "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/integer_remainder_zero")
file(MAKE_DIRECTORY "${source_panic_group6_integer_remainder_zero}")
file(GENERATE OUTPUT "${source_panic_group6_integer_remainder_zero}/root.xr" CONTENT [==[class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
var divisor: u8 = 0
if (ok) { divisor = 2 }
var result: u8 = 85 % divisor
return first.value + second.value + (result as i64) - 1
}
fn answer() -> i64 { return checked(false) }
@test
fn checkPanicFailure() { answer() }
@test
fn checkPanicSuccess() { assert(checked(true) == 42) }
]==] NEWLINE_STYLE LF)
set(source_panic_group6_integer_division_zero "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/integer_division_zero")
file(MAKE_DIRECTORY "${source_panic_group6_integer_division_zero}")
file(GENERATE OUTPUT "${source_panic_group6_integer_division_zero}/root.xr" CONTENT [==[class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
var divisor: u8 = 0
if (ok) { divisor = 2 }
var result: u8 = 84 / divisor
return first.value + second.value + (result as i64) - 42
}
fn answer() -> i64 { return checked(false) }
@test
fn checkPanicFailure() { answer() }
@test
fn checkPanicSuccess() { assert(checked(true) == 42) }
]==] NEWLINE_STYLE LF)
set(source_panic_group6_sealed_call_panic "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/sealed_call_panic")
file(MAKE_DIRECTORY "${source_panic_group6_sealed_call_panic}")
file(GENERATE OUTPUT "${source_panic_group6_sealed_call_panic}/root.xr" CONTENT [==[fn quotient(divisor: i64) -> i64 { return 84 / divisor }
class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
var divisor: i64 = 0
if (ok) { divisor = 2 }
var result = quotient(divisor)
return first.value + second.value + result - 42
}
fn answer() -> i64 { return checked(false) }
@test
fn checkPanicFailure() { answer() }
@test
fn checkPanicSuccess() { assert(checked(true) == 42) }
]==] NEWLINE_STYLE LF)
set(source_panic_group6_indirect_call_panic "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/indirect_call_panic")
file(MAKE_DIRECTORY "${source_panic_group6_indirect_call_panic}")
file(GENERATE OUTPUT "${source_panic_group6_indirect_call_panic}/root.xr" CONTENT [==[fn quotient(divisor: i64) -> i64 { return 84 / divisor }
class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
var divisor: i64 = 0
if (ok) { divisor = 2 }
var selected = quotient
var result = selected(divisor)
return first.value + second.value + result - 42
}
fn answer() -> i64 { return checked(false) }
@test
fn checkPanicFailure() { answer() }
@test
fn checkPanicSuccess() { assert(checked(true) == 42) }
]==] NEWLINE_STYLE LF)
set(source_panic_group6_sealed_panic_only_defer "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/sealed_panic_only_defer")
file(MAKE_DIRECTORY "${source_panic_group6_sealed_panic_only_defer}")
file(GENERATE OUTPUT "${source_panic_group6_sealed_panic_only_defer}/root.xr" CONTENT [==[fn quotient(divisor: i64) -> i64 { return 84 / divisor }
var cleanupCount: i64 = 0
fn readCleanupCount() -> i64 { return cleanupCount }
class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
defer { if (ok) { cleanupCount = 11 } else { cleanupCount = 22 } }
var divisor: i64 = 0
if (ok) { divisor = 2 }
var result = quotient(divisor)
return first.value + second.value + result - 42
}
fn answer() -> i64 { return checked(false) + readCleanupCount() }
@test
fn checkFreshCleanup() { assert(readCleanupCount() == 0) }
@test
fn checkPanicFailure() { answer() }
@test
fn checkFailureCleanup() { assert(readCleanupCount() == 22) }
@test
fn checkPanicSuccess() { assert(checked(true) == 42); assert(readCleanupCount() == 11) }
]==] NEWLINE_STYLE LF)
set(source_panic_group6_indirect_panic_only_defer "${CMAKE_BINARY_DIR}/generated/source-builtin-panic-group6/indirect_panic_only_defer")
file(MAKE_DIRECTORY "${source_panic_group6_indirect_panic_only_defer}")
file(GENERATE OUTPUT "${source_panic_group6_indirect_panic_only_defer}/root.xr" CONTENT [==[fn quotient(divisor: i64) -> i64 { return 84 / divisor }
var cleanupCount: i64 = 0
fn readCleanupCount() -> i64 { return cleanupCount }
class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
defer { if (ok) { cleanupCount = 11 } else { cleanupCount = 22 } }
var divisor: i64 = 0
if (ok) { divisor = 2 }
var selected = quotient
var result = selected(divisor)
return first.value + second.value + result - 42
}
fn answer() -> i64 { return checked(false) + readCleanupCount() }
@test
fn checkFreshCleanup() { assert(readCleanupCount() == 0) }
@test
fn checkPanicFailure() { answer() }
@test
fn checkFailureCleanup() { assert(readCleanupCount() == 22) }
@test
fn checkPanicSuccess() { assert(checked(true) == 42); assert(readCleanupCount() == 11) }
]==] NEWLINE_STYLE LF)
add_executable(test_source_product_builtin_panic_group6
    "${source_panic_group6_dir}/test_source_product_builtin_panic_group6.c")
target_link_libraries(test_source_product_builtin_panic_group6 PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_builtin_panic_group6 PRIVATE "${source_panic_group6_dir}/..")
set_target_properties(test_source_product_builtin_panic_group6 PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_builtin_panic_group6 PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_builtin_panic_group6 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_builtin_panic_group6 PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_builtin_panic_group6_integer_remainder_zero_vm_normal COMMAND test_source_product_builtin_panic_group6 integer_remainder_zero
    "${source_panic_group6_integer_remainder_zero}" "${source_panic_group6_integer_remainder_zero}/root.xr")
set_tests_properties(test_source_product_builtin_panic_group6_integer_remainder_zero_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;numeric-panic;ownership;vm-projection")
add_test(NAME test_source_product_builtin_panic_group6_integer_division_zero_vm_normal COMMAND test_source_product_builtin_panic_group6 integer_division_zero
    "${source_panic_group6_integer_division_zero}" "${source_panic_group6_integer_division_zero}/root.xr")
set_tests_properties(test_source_product_builtin_panic_group6_integer_division_zero_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;numeric-panic;ownership;vm-projection")
add_test(NAME test_source_product_builtin_panic_group6_sealed_call_panic_vm_normal COMMAND test_source_product_builtin_panic_group6 sealed_call_panic
    "${source_panic_group6_sealed_call_panic}" "${source_panic_group6_sealed_call_panic}/root.xr")
set_tests_properties(test_source_product_builtin_panic_group6_sealed_call_panic_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;numeric-panic;ownership;vm-projection")
add_test(NAME test_source_product_builtin_panic_group6_indirect_call_panic_vm_normal COMMAND test_source_product_builtin_panic_group6 indirect_call_panic
    "${source_panic_group6_indirect_call_panic}" "${source_panic_group6_indirect_call_panic}/root.xr")
set_tests_properties(test_source_product_builtin_panic_group6_indirect_call_panic_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;numeric-panic;ownership;vm-projection")
add_test(NAME test_source_product_builtin_panic_group6_sealed_panic_only_defer_vm_normal COMMAND test_source_product_builtin_panic_group6 sealed_panic_only_defer
    "${source_panic_group6_sealed_panic_only_defer}" "${source_panic_group6_sealed_panic_only_defer}/root.xr")
set_tests_properties(test_source_product_builtin_panic_group6_sealed_panic_only_defer_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;numeric-panic;ownership;vm-projection")
add_test(NAME test_source_product_builtin_panic_group6_indirect_panic_only_defer_vm_normal COMMAND test_source_product_builtin_panic_group6 indirect_panic_only_defer
    "${source_panic_group6_indirect_panic_only_defer}" "${source_panic_group6_indirect_panic_only_defer}/root.xr")
set_tests_properties(test_source_product_builtin_panic_group6_indirect_panic_only_defer_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;numeric-panic;ownership;vm-projection")
