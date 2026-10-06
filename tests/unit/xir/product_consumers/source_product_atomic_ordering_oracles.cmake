# Private test roles prove independent Atomic values and exact ordering controls.
set(atomic_ordering_dir "${CMAKE_CURRENT_LIST_DIR}")
set(atomic_ordering_fixture "${CMAKE_BINARY_DIR}/generated/atomic-ordering-oracles")
file(MAKE_DIRECTORY "${atomic_ordering_fixture}")
file(GENERATE OUTPUT "${atomic_ordering_fixture}/rmw_Relaxed.xr" CONTENT [==[fn answer() -> i64 {
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.Relaxed)
 return old + counter.load(Ordering.SeqCst)
}
@test
fn checkOrdering() {
 assert(answer() == 82)
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.Relaxed)
 const current = counter.load(Ordering.SeqCst)
 assert(old == 42)
 assert(current == 40)
}
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${atomic_ordering_fixture}/rmw_Acquire.xr" CONTENT [==[fn answer() -> i64 {
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.Acquire)
 return old + counter.load(Ordering.SeqCst)
}
@test
fn checkOrdering() {
 assert(answer() == 82)
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.Acquire)
 const current = counter.load(Ordering.SeqCst)
 assert(old == 42)
 assert(current == 40)
}
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${atomic_ordering_fixture}/rmw_Release.xr" CONTENT [==[fn answer() -> i64 {
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.Release)
 return old + counter.load(Ordering.SeqCst)
}
@test
fn checkOrdering() {
 assert(answer() == 82)
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.Release)
 const current = counter.load(Ordering.SeqCst)
 assert(old == 42)
 assert(current == 40)
}
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${atomic_ordering_fixture}/rmw_AcquireRelease.xr" CONTENT [==[fn answer() -> i64 {
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.AcquireRelease)
 return old + counter.load(Ordering.SeqCst)
}
@test
fn checkOrdering() {
 assert(answer() == 82)
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.AcquireRelease)
 const current = counter.load(Ordering.SeqCst)
 assert(old == 42)
 assert(current == 40)
}
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${atomic_ordering_fixture}/rmw_SeqCst.xr" CONTENT [==[fn answer() -> i64 {
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.SeqCst)
 return old + counter.load(Ordering.SeqCst)
}
@test
fn checkOrdering() {
 assert(answer() == 82)
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.SeqCst)
 const current = counter.load(Ordering.SeqCst)
 assert(old == 42)
 assert(current == 40)
}
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${atomic_ordering_fixture}/original_Relaxed.xr" CONTENT [==[fn answer() -> i64 {
 const counter = Atomic(40)
 counter.store(42, Ordering.Relaxed)
 const old = counter.fetchSub(2, Ordering.Relaxed)
 return old + counter.load(Ordering.Relaxed)
}
@test
fn checkOrdering() {
 assert(answer() == 82)
 const counter = Atomic(40)
 counter.store(42, Ordering.Relaxed)
 const old = counter.fetchSub(2, Ordering.Relaxed)
 const current = counter.load(Ordering.Relaxed)
 assert(old == 42)
 assert(current == 40)
}
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${atomic_ordering_fixture}/original_SeqCst.xr" CONTENT [==[fn answer() -> i64 {
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.SeqCst)
 return old + counter.load(Ordering.SeqCst)
}
@test
fn checkOrdering() {
 assert(answer() == 82)
 const counter = Atomic(40)
 counter.store(42, Ordering.SeqCst)
 const old = counter.fetchSub(2, Ordering.SeqCst)
 const current = counter.load(Ordering.SeqCst)
 assert(old == 42)
 assert(current == 40)
}
]==] NEWLINE_STYLE LF)
add_executable(test_source_product_atomic_ordering_oracles
    "${atomic_ordering_dir}/test_source_product_atomic_ordering_oracles.c")
target_link_libraries(test_source_product_atomic_ordering_oracles PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_atomic_ordering_oracles PRIVATE "${atomic_ordering_dir}/..")
set_target_properties(test_source_product_atomic_ordering_oracles PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_atomic_ordering_oracles PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_atomic_ordering_oracles PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_atomic_ordering_oracles PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case IN ITEMS rmw_Relaxed rmw_Acquire rmw_Release rmw_AcquireRelease rmw_SeqCst original_Relaxed original_SeqCst)
    set(test_name "test_source_product_atomic_ordering_${case}_normal")
    add_test(NAME "${test_name}" COMMAND test_source_product_atomic_ordering_oracles
        "${case}" "${atomic_ordering_fixture}" "${atomic_ordering_fixture}/${case}.xr")
    set_tests_properties("${test_name}" PROPERTIES TIMEOUT 120
        LABELS "unit;xir;source-product;program-consumer;atomic;ownership;vm-projection")
endforeach()
