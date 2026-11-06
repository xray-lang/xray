# Direct storage tests compile the same owner used by the legacy VM.

# This legacy context includes pre-existing flexible-array and conversion
# diagnostics. Inherit its canonical warning policy without suppressions.

enable_testing()

xray_stage_windows_asan_runtime("${CMAKE_CURRENT_BINARY_DIR}")
xray_prepend_windows_asan_runtime_to_directory_tests()
