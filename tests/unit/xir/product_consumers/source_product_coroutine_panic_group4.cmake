# Original coroutine panic families; one independent finite process per complete input.
set(source_coroutine_panic_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_coroutine_panic_sealed_coroutine_invoke "${CMAKE_BINARY_DIR}/generated/source-coroutine-panic-group4/sealed_coroutine_invoke")
file(MAKE_DIRECTORY "${source_coroutine_panic_sealed_coroutine_invoke}")
file(GENERATE OUTPUT "${source_coroutine_panic_sealed_coroutine_invoke}/root.xr" CONTENT [==[enum DivisionFailure { Negative }
fn quotient(divisor: i64) -> i64 {
Coro.yield()
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
set(source_coroutine_panic_indirect_coroutine_invoke "${CMAKE_BINARY_DIR}/generated/source-coroutine-panic-group4/indirect_coroutine_invoke")
file(MAKE_DIRECTORY "${source_coroutine_panic_indirect_coroutine_invoke}")
file(GENERATE OUTPUT "${source_coroutine_panic_indirect_coroutine_invoke}/root.xr" CONTENT [==[enum DivisionFailure { Negative }
fn quotient(divisor: i64) -> i64 {
Coro.yield()
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
set(source_coroutine_panic_sealed_coroutine_panic "${CMAKE_BINARY_DIR}/generated/source-coroutine-panic-group4/sealed_coroutine_panic")
file(MAKE_DIRECTORY "${source_coroutine_panic_sealed_coroutine_panic}")
file(GENERATE OUTPUT "${source_coroutine_panic_sealed_coroutine_panic}/root.xr" CONTENT [==[fn quotient(divisor: i64) -> i64 {
Coro.yield()
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
set(source_coroutine_panic_indirect_coroutine_panic "${CMAKE_BINARY_DIR}/generated/source-coroutine-panic-group4/indirect_coroutine_panic")
file(MAKE_DIRECTORY "${source_coroutine_panic_indirect_coroutine_panic}")
file(GENERATE OUTPUT "${source_coroutine_panic_indirect_coroutine_panic}/root.xr" CONTENT [==[fn quotient(divisor: i64) -> i64 {
Coro.yield()
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

add_executable(test_source_product_coroutine_panic_group4
    "${source_coroutine_panic_dir}/test_source_product_coroutine_panic_group4.c")
target_link_libraries(test_source_product_coroutine_panic_group4 PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_coroutine_panic_group4 PRIVATE "${source_coroutine_panic_dir}/..")
set_target_properties(test_source_product_coroutine_panic_group4 PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_coroutine_panic_group4 PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_coroutine_panic_group4 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_coroutine_panic_group4 PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_coroutine_panic_group4_sealed_coroutine_invoke_vm_normal COMMAND test_source_product_coroutine_panic_group4 sealed_coroutine_invoke "${source_coroutine_panic_sealed_coroutine_invoke}" "${source_coroutine_panic_sealed_coroutine_invoke}/root.xr")
set_tests_properties(test_source_product_coroutine_panic_group4_sealed_coroutine_invoke_vm_normal PROPERTIES TIMEOUT 120 PROCESSORS 1)
add_test(NAME test_source_product_coroutine_panic_group4_indirect_coroutine_invoke_vm_normal COMMAND test_source_product_coroutine_panic_group4 indirect_coroutine_invoke "${source_coroutine_panic_indirect_coroutine_invoke}" "${source_coroutine_panic_indirect_coroutine_invoke}/root.xr")
set_tests_properties(test_source_product_coroutine_panic_group4_indirect_coroutine_invoke_vm_normal PROPERTIES TIMEOUT 120 PROCESSORS 1)
add_test(NAME test_source_product_coroutine_panic_group4_sealed_coroutine_panic_vm_normal COMMAND test_source_product_coroutine_panic_group4 sealed_coroutine_panic "${source_coroutine_panic_sealed_coroutine_panic}" "${source_coroutine_panic_sealed_coroutine_panic}/root.xr")
set_tests_properties(test_source_product_coroutine_panic_group4_sealed_coroutine_panic_vm_normal PROPERTIES TIMEOUT 120 PROCESSORS 1)
add_test(NAME test_source_product_coroutine_panic_group4_indirect_coroutine_panic_vm_normal COMMAND test_source_product_coroutine_panic_group4 indirect_coroutine_panic "${source_coroutine_panic_indirect_coroutine_panic}" "${source_coroutine_panic_indirect_coroutine_panic}/root.xr")
set_tests_properties(test_source_product_coroutine_panic_group4_indirect_coroutine_panic_vm_normal PROPERTIES TIMEOUT 120 PROCESSORS 1)
