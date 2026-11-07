

set(CHANNEL_BLOCK_NATIVE "${CMAKE_CURRENT_BINARY_DIR}/channel-blocking-native.c")

add_test(NAME test_xir_channel_blocking_checked_vectors COMMAND ${Python3_EXECUTABLE}
    "${PROJECT_SOURCE_DIR}/tests/unit/xir/derive_blocking_checked75_goldens.py")
set_tests_properties(test_xir_channel_blocking_checked_vectors PROPERTIES LABELS "unit;xir;channel;checked;metadata" TIMEOUT 30)

foreach(direction vm-native native-vm)
    string(REPLACE "-" "_" suffix "${direction}")

endforeach()

include("${CMAKE_CURRENT_LIST_DIR}/channel_timed_execution.cmake")
