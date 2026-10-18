# Every complete root command keeps its original independent deadline.
add_test(NAME test_root_union_replay_storage_values COMMAND test_xir_root_conditional_helpers --replay-storage-values)
set_tests_properties(test_root_union_replay_storage_values PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_replay_storage_faults COMMAND test_xir_root_conditional_helpers --replay-storage-faults)
set_tests_properties(test_root_union_replay_storage_faults PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_read_allocation_bounds COMMAND test_xir_root_conditional_helpers --read-allocation-bounds)
set_tests_properties(test_root_union_read_allocation_bounds PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_nonroot_input_seeds_resources COMMAND test_xir_root_conditional_helpers --nonroot-input-seeds-resources)
set_tests_properties(test_root_union_nonroot_input_seeds_resources PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_nonroot_input_seeds_literals COMMAND test_xir_root_conditional_helpers --nonroot-input-seeds-literals)
set_tests_properties(test_root_union_nonroot_input_seeds_literals PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_final_retirement_literals COMMAND test_xir_root_conditional_helpers --final-retirement-literals)
set_tests_properties(test_root_union_final_retirement_literals PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_ordinary_environment_literals COMMAND test_xir_root_conditional_helpers --ordinary-environment-literals)
set_tests_properties(test_root_union_ordinary_environment_literals PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_ordinary_environment_keys COMMAND test_xir_root_conditional_helpers --ordinary-environment-keys)
set_tests_properties(test_root_union_ordinary_environment_keys PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_final_retirement COMMAND test_xir_root_conditional_helpers --final-retirement)
set_tests_properties(test_root_union_final_retirement PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_retry_census COMMAND test_xir_root_conditional_helpers --retry-census)
set_tests_properties(test_root_union_retry_census PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_rotation COMMAND test_xir_root_conditional_helpers --rotation)
set_tests_properties(test_root_union_rotation PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_rotation_pipeline COMMAND test_xir_root_conditional_helpers --rotation-pipeline)
set_tests_properties(test_root_union_rotation_pipeline PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
add_test(NAME test_root_union_new COMMAND test_xir_root_conditional_helpers --new)
set_tests_properties(test_root_union_new PROPERTIES
    TIMEOUT 600 RUN_SERIAL TRUE LABELS "unit;xir;ownership;root-effects;complete-union")
