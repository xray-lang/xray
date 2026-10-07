# A private same-module test proves both CAS fields and cell identity.
set(atomic_cas_dir "${CMAKE_CURRENT_LIST_DIR}")
set(atomic_cas_fixture "${CMAKE_BINARY_DIR}/generated/atomic-i64-cas-oracles")
file(MAKE_DIRECTORY "${atomic_cas_fixture}")
file(GENERATE OUTPUT "${atomic_cas_fixture}/scenario1-cas.xr" CONTENT [==[fn answer() -> i64 {
 const counter = Atomic(40)
 const alias = counter
 var (old, matched) = alias.compareExchange(40, 42)
 counter.store(42, Ordering.Release)
 counter.add(1)
 counter.sub(1)
 return counter.load()
}
fn exported() -> i64 { return 42 }
@test
fn checkAnswer() { assert(exported() == 42) }
@test
fn checkCompareExchange() {
 assert(answer() == 42)
 const counter = Atomic(40)
 const alias = counter
 const independent = Atomic(40)
 var (successOld, successMatched) = alias.compareExchange(40, 42)
 assert(successOld == 40)
 assert(successMatched == true)
 assert(counter.load() == 42)
 assert(alias.load() == 42)
 assert(independent.load() == 40)
 var (failureOld, failureMatched) = counter.compareExchange(40, 99)
 assert(failureOld == 42)
 assert(failureMatched == false)
 assert(counter.load() == 42)
 assert(alias.load() == 42)
 assert(independent.load() == 40)
}
]==] NEWLINE_STYLE LF)
add_executable(test_source_product_atomic_i64_cas_oracles "${atomic_cas_dir}/test_source_product_atomic_i64_cas_oracles.c")
target_link_libraries(test_source_product_atomic_i64_cas_oracles PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_atomic_i64_cas_oracles PRIVATE "${atomic_cas_dir}/..")
set_target_properties(test_source_product_atomic_i64_cas_oracles PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_atomic_i64_cas_oracles PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_atomic_i64_cas_oracles PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_atomic_i64_cas_oracles PRIVATE -Wall -Wextra -Werror)
endif()
add_test(NAME test_source_product_atomic_i64_cas_normal COMMAND test_source_product_atomic_i64_cas_oracles
    "i64_cas" "${atomic_cas_fixture}" "${atomic_cas_fixture}/scenario1-cas.xr")
set_tests_properties(test_source_product_atomic_i64_cas_normal PROPERTIES TIMEOUT 120
    LABELS "unit;xir;source-product;program-consumer;atomic;ownership;vm-projection")
