# Original typed error/panic and mutual-call families; independent finite process per complete input.
set(source_call_error_panic_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_call_error_panic_sealed_invoke_panic "${CMAKE_BINARY_DIR}/generated/source-call-error-panic-group5/sealed_invoke_panic")
file(MAKE_DIRECTORY "${source_call_error_panic_sealed_invoke_panic}")
file(GENERATE OUTPUT "${source_call_error_panic_sealed_invoke_panic}/root.xr" CONTENT [==[enum DivisionFailure { Negative }
fn quotient(divisor: i64) -> i64 {
if (divisor < 0) { throw DivisionFailure.Negative }
return 84 / divisor
}
class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
var divisor: i64 = 0
if (ok) { divisor = 2 }
var result = 0
try { result = quotient(divisor) }
catch (error: DivisionFailure) { result = 7 }
var caughtResult = 0
try { caughtResult = quotient(-1) }
catch (error: DivisionFailure) { caughtResult = 7 }
assert(caughtResult == 7)
return first.value + second.value + result - 42
}
fn answer() -> i64 { return checked(false) }
@test
fn checkPanicFailure() { answer() }
@test
fn checkPanicSuccess() { assert(checked(true) == 42) }
]==] NEWLINE_STYLE LF)
set(source_call_error_panic_indirect_invoke_panic "${CMAKE_BINARY_DIR}/generated/source-call-error-panic-group5/indirect_invoke_panic")
file(MAKE_DIRECTORY "${source_call_error_panic_indirect_invoke_panic}")
file(GENERATE OUTPUT "${source_call_error_panic_indirect_invoke_panic}/root.xr" CONTENT [==[enum DivisionFailure { Negative }
fn quotient(divisor: i64) -> i64 {
if (divisor < 0) { throw DivisionFailure.Negative }
return 84 / divisor
}
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
var result = 0
try { result = selected(divisor) }
catch (error: DivisionFailure) { result = 7 }
var caughtResult = 0
try { caughtResult = selected(-1) }
catch (error: DivisionFailure) { caughtResult = 7 }
assert(caughtResult == 7)
return first.value + second.value + result - 42
}
fn answer() -> i64 { return checked(false) }
@test
fn checkPanicFailure() { answer() }
@test
fn checkPanicSuccess() { assert(checked(true) == 42) }
]==] NEWLINE_STYLE LF)
set(source_call_error_panic_sealed_invoke_defer "${CMAKE_BINARY_DIR}/generated/source-call-error-panic-group5/sealed_invoke_defer")
file(MAKE_DIRECTORY "${source_call_error_panic_sealed_invoke_defer}")
file(GENERATE OUTPUT "${source_call_error_panic_sealed_invoke_defer}/root.xr" CONTENT [==[enum DivisionFailure { Negative }
fn quotient(divisor: i64) -> i64 {
if (divisor < 0) { throw DivisionFailure.Negative }
return 84 / divisor
}
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
var result = 0
try { result = quotient(divisor) }
catch (error: DivisionFailure) { result = 7 }
var caughtResult = 0
try { caughtResult = quotient(-1) }
catch (error: DivisionFailure) { caughtResult = 7 }
assert(caughtResult == 7)
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
set(source_call_error_panic_indirect_invoke_defer "${CMAKE_BINARY_DIR}/generated/source-call-error-panic-group5/indirect_invoke_defer")
file(MAKE_DIRECTORY "${source_call_error_panic_indirect_invoke_defer}")
file(GENERATE OUTPUT "${source_call_error_panic_indirect_invoke_defer}/root.xr" CONTENT [==[enum DivisionFailure { Negative }
fn quotient(divisor: i64) -> i64 {
if (divisor < 0) { throw DivisionFailure.Negative }
return 84 / divisor
}
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
var result = 0
try { result = selected(divisor) }
catch (error: DivisionFailure) { result = 7 }
var caughtResult = 0
try { caughtResult = selected(-1) }
catch (error: DivisionFailure) { caughtResult = 7 }
assert(caughtResult == 7)
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
set(source_call_error_panic_recursive_call_panic "${CMAKE_BINARY_DIR}/generated/source-call-error-panic-group5/recursive_call_panic")
file(MAKE_DIRECTORY "${source_call_error_panic_recursive_call_panic}")
file(GENERATE OUTPUT "${source_call_error_panic_recursive_call_panic}/root.xr" CONTENT [==[fn quotient(depth: i64, divisor: i64) -> i64 {
if (depth == 0) { return 84 / divisor }
return relay(depth - 1, divisor)
}
fn relay(depth: i64, divisor: i64) -> i64 {
return quotient(depth, divisor)
}
class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
var divisor: i64 = 0
if (ok) { divisor = 2 }
var result = quotient(3, divisor)
return first.value + second.value + result - 42
}
fn answer() -> i64 { return checked(false) }
@test
fn checkPanicFailure() { answer() }
@test
fn checkPanicSuccess() { assert(checked(true) == 42) }
]==] NEWLINE_STYLE LF)
add_executable(test_source_product_call_error_panic_group5
    "${source_call_error_panic_dir}/test_source_product_call_error_panic_group5.c")
target_link_libraries(test_source_product_call_error_panic_group5 PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_call_error_panic_group5 PRIVATE "${source_call_error_panic_dir}/..")
set_target_properties(test_source_product_call_error_panic_group5 PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_call_error_panic_group5 PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_call_error_panic_group5 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_call_error_panic_group5 PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_call_error_panic_group5_sealed_invoke_panic_vm_normal COMMAND test_source_product_call_error_panic_group5 sealed_invoke_panic
    "${source_call_error_panic_sealed_invoke_panic}" "${source_call_error_panic_sealed_invoke_panic}/root.xr")
set_tests_properties(test_source_product_call_error_panic_group5_sealed_invoke_panic_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;error-panic;ownership;vm-projection")
add_test(NAME test_source_product_call_error_panic_group5_indirect_invoke_panic_vm_normal COMMAND test_source_product_call_error_panic_group5 indirect_invoke_panic
    "${source_call_error_panic_indirect_invoke_panic}" "${source_call_error_panic_indirect_invoke_panic}/root.xr")
set_tests_properties(test_source_product_call_error_panic_group5_indirect_invoke_panic_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;error-panic;ownership;vm-projection")
add_test(NAME test_source_product_call_error_panic_group5_sealed_invoke_defer_vm_normal COMMAND test_source_product_call_error_panic_group5 sealed_invoke_defer
    "${source_call_error_panic_sealed_invoke_defer}" "${source_call_error_panic_sealed_invoke_defer}/root.xr")
set_tests_properties(test_source_product_call_error_panic_group5_sealed_invoke_defer_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;error-panic;ownership;vm-projection")
add_test(NAME test_source_product_call_error_panic_group5_indirect_invoke_defer_vm_normal COMMAND test_source_product_call_error_panic_group5 indirect_invoke_defer
    "${source_call_error_panic_indirect_invoke_defer}" "${source_call_error_panic_indirect_invoke_defer}/root.xr")
set_tests_properties(test_source_product_call_error_panic_group5_indirect_invoke_defer_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;error-panic;ownership;vm-projection")
add_test(NAME test_source_product_call_error_panic_group5_recursive_call_panic_vm_normal COMMAND test_source_product_call_error_panic_group5 recursive_call_panic
    "${source_call_error_panic_recursive_call_panic}" "${source_call_error_panic_recursive_call_panic}/root.xr")
set_tests_properties(test_source_product_call_error_panic_group5_recursive_call_panic_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;error-panic;ownership;vm-projection")
