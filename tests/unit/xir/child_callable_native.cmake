# One shared graph is compiled only in the producer; native bodies stay runtime-only.
set(CN_NATIVE_C "${CMAKE_BINARY_DIR}/generated/child_callable_native.c")
add_executable(test_xir_child_callable_native_producer
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_child_callable_native_producer.c")
target_link_libraries(test_xir_child_callable_native_producer PRIVATE xray_xir_vm xray_xir_cgen xray_xir_scalar)
add_custom_command(OUTPUT "${CN_NATIVE_C}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
    COMMAND $<TARGET_FILE:test_xir_child_callable_native_producer> "${CN_NATIVE_C}"
    DEPENDS test_xir_child_callable_native_producer
        "${PROJECT_SOURCE_DIR}/tests/unit/xir/xir_child_callable_permissions.h" VERBATIM)
add_executable(test_xir_child_callable_native
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_child_callable_native.c" "${CN_NATIVE_C}")
target_link_libraries(test_xir_child_callable_native PRIVATE xray_xir_scalar)
xr_enable_pure_aot_symbol_map(test_xir_child_callable_native)
foreach(target test_xir_child_callable_native_producer test_xir_child_callable_native)
    set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    if(MSVC)
        target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
set(CN_NATIVE_VERIFY [=[
import re,subprocess,sys
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from check_xr_program_aot_native import forbidden_symbol_family,load_symbol_inventory
exe=Path(sys.argv[2]).resolve()
run=subprocess.run([str(exe)],capture_output=True,timeout=110)
assert run.returncode==0,(run.returncode,run.stdout,run.stderr)
expected=b'NATIVE_CHILD_LOCAL ordinaryNone=2 capturedNone=2 rootUnknownMixedDenied=8 crossInstanceDenied=2 forgedDenied=6 typed42=2 bytes42=2 Instances=2 compilerRuntimePhysical=0/0 mixedFIresources=OPEN\n'
assert run.stdout.replace(b'\r\n',b'\n')==expected,(run.stdout,expected)
assert re.fullmatch(rb'effects Source compiler: 1 finite owners, max allocated=\d+ peak=\d+ work=\d+; physical=0/0\r?\n',run.stderr),run.stderr
symbols,error=load_symbol_inventory(exe)
assert symbols is not None,error
assert forbidden_symbol_family(symbols) is None,'legacy compiler/VM linked'
assert not re.search(r'(?<![A-Za-z0-9_])(?:__imp_)?_*xr_xir_(?:vm_|compile_(?:source_|specialize|emit_c|vm_))',symbols,re.I),'Source/emitter/VM linked'
print(expected.decode().strip())
print('NATIVE_CHILD_SYMBOLS actual generated C11, no legacy/Source/emitter/VM')
]=])
add_test(NAME test_xir_child_callable_native COMMAND "${Python3_EXECUTABLE}" -B -c "${CN_NATIVE_VERIFY}"
    "${PROJECT_SOURCE_DIR}/scripts" $<TARGET_FILE:test_xir_child_callable_native>)
set_tests_properties(test_xir_child_callable_native PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
    LABELS "unit;xir;task;root-effects;ownership;instance;generated-c;portability;abi")
