# Reuse the one canonical producer and its real generated C output.
if(NOT TARGET test_xir_child_callable_native_producer OR NOT DEFINED CN_NATIVE_C)
    message(FATAL_ERROR "child_callable_native.cmake must be included before the mixed consumer")
endif()
add_executable(test_xir_child_callable_mixed
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/test_xir_child_callable_mixed.c" "${CN_NATIVE_C}")
target_link_libraries(test_xir_child_callable_mixed PRIVATE xray_xir_vm xray_xir_scalar)
xr_enable_pure_aot_symbol_map(test_xir_child_callable_mixed)
set_target_properties(test_xir_child_callable_mixed PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_xir_child_callable_mixed PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_xir_child_callable_mixed PRIVATE -Wall -Wextra -Werror)
endif()
set(CM_MIXED_VERIFY [=[
import re,subprocess,sys
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from check_xr_program_aot_native import forbidden_symbol_family,load_symbol_inventory
exe=Path(sys.argv[2]).resolve()
mode=sys.argv[3]
assert mode in ('vm-native','native-vm'),mode
run=subprocess.run([str(exe),mode],capture_output=True,timeout=110)
assert run.returncode==0,(run.returncode,run.stdout,run.stderr)
expected=('MIXED_CHILD_LOCAL direction='+mode+' rootSteps=6 childSteps=10 relaySteps=4 pureSteps=4 rootAwait=2 childRelay=2 relayPure=2 ordinaryNone=2 capturedNone=2 rootUnknownMixedDenied=8 crossInstanceDenied=2 forgedDenied=6 occupiedUnchanged=2 typed42=2 bytes42=2 Instances=2 compilerRuntimePhysical=0/0 FIresources=OPEN\n').encode()
assert run.stdout.replace(b'\r\n',b'\n')==expected,(run.stdout,expected)
assert re.fullmatch(rb'effects Source compiler: 1 finite owners, max allocated=\d+ peak=\d+ work=\d+; physical=0/0\r?\n',run.stderr),run.stderr
symbols,error=load_symbol_inventory(exe)
assert symbols is not None,error
assert forbidden_symbol_family(symbols) is None,'legacy compiler/VM linked'
assert not re.search(r'(?<![A-Za-z0-9_])(?:__imp_)?_*xr_xir_compile_(?:source_|emit_c)',symbols,re.I),'Source frontend/emitter linked'
for symbol in ('xr_xir_compile_vm_bind','child_callable_native_f2','child_callable_native_f3','child_callable_native_f4','child_callable_native_f5'):
    assert re.search(r'(?<![A-Za-z0-9_])_*'+re.escape(symbol)+r'(?![A-Za-z0-9_])',symbols),symbol
print(expected.decode().strip())
print('MIXED_CHILD_SYMBOLS actual native bodies and VM bind, no legacy/Source/emitter')
]=])
foreach(direction vm-native native-vm)
    string(REPLACE "-" "_" test_direction "${direction}")
    add_test(NAME test_xir_child_callable_mixed_${test_direction}
        COMMAND "${Python3_EXECUTABLE}" -B -c "${CM_MIXED_VERIFY}"
            "${PROJECT_SOURCE_DIR}/scripts" $<TARGET_FILE:test_xir_child_callable_mixed> "${direction}")
    set_tests_properties(test_xir_child_callable_mixed_${test_direction} PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
        LABELS "unit;xir;task;root-effects;ownership;instance;generated-c;mixed;portability;abi")
endforeach()
