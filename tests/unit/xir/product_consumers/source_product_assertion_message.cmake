# Complete original assertion source executes only through private same-module test roles.
set(source_assertion_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_assertion_fixture "${CMAKE_BINARY_DIR}/generated/source-assertion-message")
file(MAKE_DIRECTORY "${source_assertion_fixture}")
file(GENERATE OUTPUT "${source_assertion_fixture}/root.xr" CONTENT [==[var cleanupCount: i64 = 0
fn readCleanupCount() -> i64 { return cleanupCount }
class Cell {
value: i64
constructor(value: i64) { this.value = value }
}
fn checked(ok: bool) -> i64 {
var first = Cell(20)
var second = Cell(22)
defer { if (ok) { cleanupCount = 11 } else { cleanupCount = 22 } }
var message = "assert " + "message"
assert(ok, message)
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
fn checkSuccessCleanup() { assert(checked(true) == 42); assert(readCleanupCount() == 11) }
]==] NEWLINE_STYLE LF)
add_executable(test_source_product_assertion_message
    "${source_assertion_dir}/test_source_product_assertion_message.c")
target_link_libraries(test_source_product_assertion_message PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_assertion_message PRIVATE "${source_assertion_dir}/..")
set_target_properties(test_source_product_assertion_message PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_assertion_message PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_assertion_message PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_assertion_message PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_assertion_message_vm_normal COMMAND test_source_product_assertion_message
    "${source_assertion_fixture}" "${source_assertion_fixture}/root.xr")
set_tests_properties(test_source_product_assertion_message_vm_normal PROPERTIES
    TIMEOUT 120 PROCESSORS 1
    LABELS "unit;xir;source-product;program-consumer;assertion;ownership;vm-projection")
