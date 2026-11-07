# Keep the original scenario-zero target and every other module-state target.
# The source-owned product supplies both independently checked execution modes.
set(XIR_STATE_ZERO_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_module_state_zero.c)
set(XIR_STATE_ZERO_FIXTURE ${CMAKE_SOURCE_DIR}/tests/fixtures/xir_module_state_zero)
