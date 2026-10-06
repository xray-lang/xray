# Exact source families execute private Atomic bodies through test roles.
set(source_atomic_dir "${CMAKE_CURRENT_LIST_DIR}")
set(source_atomic_fixture "${CMAKE_BINARY_DIR}/generated/source-atomic-families")
file(MAKE_DIRECTORY "${source_atomic_fixture}")
file(GENERATE OUTPUT "${source_atomic_fixture}/scenario1-base.xr" CONTENT [==[fn answer() -> i64 {
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
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${source_atomic_fixture}/scenario1-atomic.xr" CONTENT [==[fn answer() -> i64 {
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
fn checkAtomicAnswer() { assert(answer() == 42) }
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${source_atomic_fixture}/scenario2-base.xr" CONTENT [==[fn answer() -> i64 {
 const flag = Atomic(false)
 const alias = flag
 var (old, matched) = alias.compareExchange(false, true)
 flag.store(true, Ordering.Release)
 const before = flag.toggle(Ordering.Relaxed)
 if (matched && before) { return 42 }
 return 0
}
fn exported() -> i64 { return 42 }
@test
fn checkAnswer() { assert(exported() == 42) }
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${source_atomic_fixture}/scenario2-atomic.xr" CONTENT [==[fn answer() -> i64 {
 const flag = Atomic(false)
 const alias = flag
 var (old, matched) = alias.compareExchange(false, true)
 flag.store(true, Ordering.Release)
 const before = flag.toggle(Ordering.Relaxed)
 if (matched && before) { return 42 }
 return 0
}
fn exported() -> i64 { return 42 }
@test
fn checkAnswer() { assert(exported() == 42) }
@test
fn checkAtomicAnswer() { assert(answer() == 42) }
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${source_atomic_fixture}/scenario3-base.xr" CONTENT [==[fn identity(value: f64) -> f64 { return value }
fn answer() -> i64 {
 const value = identity(1.5)
 const other = identity(2.25)
 const counter = Atomic(value)
 const old = counter.fetchAdd(other)
 var (before, matched) = counter.compareExchange(3.75, 1.5)
 counter.store(1.5, Ordering.Release)
 const text = counter.load().toString()
 if (old < other && len(text) == 3) { return 42 }
 return 0
}
fn exported() -> i64 { return 42 }
@test
fn checkAnswer() { assert(exported() == 42) }
]==] NEWLINE_STYLE LF)
file(GENERATE OUTPUT "${source_atomic_fixture}/scenario3-atomic.xr" CONTENT [==[fn identity(value: f64) -> f64 { return value }
fn answer() -> i64 {
 const value = identity(1.5)
 const other = identity(2.25)
 const counter = Atomic(value)
 const old = counter.fetchAdd(other)
 var (before, matched) = counter.compareExchange(3.75, 1.5)
 counter.store(1.5, Ordering.Release)
 const text = counter.load().toString()
 if (old < other && len(text) == 3) { return 42 }
 return 0
}
fn exported() -> i64 { return 42 }
@test
fn checkAnswer() { assert(exported() == 42) }
@test
fn checkAtomicAnswer() { assert(answer() == 42) }
]==] NEWLINE_STYLE LF)
add_executable(test_source_product_atomic_source_families
    "${source_atomic_dir}/test_source_product_atomic_source_families.c")
target_link_libraries(test_source_product_atomic_source_families PRIVATE xray_xir_source_product)
target_include_directories(test_source_product_atomic_source_families PRIVATE "${source_atomic_dir}/..")
set_target_properties(test_source_product_atomic_source_families PROPERTIES
    C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_atomic_source_families PRIVATE /utf-8 /W4 /WX)
    target_compile_definitions(test_source_product_atomic_source_families PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_source_product_atomic_source_families PRIVATE -Wall -Wextra -Werror)
endif()
foreach(scenario IN ITEMS 1 2 3)
    foreach(mode IN ITEMS base atomic)
        set(test_name "test_source_product_atomic_source${scenario}_${mode}_normal")
        add_test(NAME "${test_name}" COMMAND test_source_product_atomic_source_families
            "${scenario}" "${mode}" "${source_atomic_fixture}"
            "${source_atomic_fixture}/scenario${scenario}-${mode}.xr")
        set_tests_properties("${test_name}" PROPERTIES TIMEOUT 120
            LABELS "unit;xir;source-product;program-consumer;atomic;ownership;vm-projection")
    endforeach()
endforeach()
