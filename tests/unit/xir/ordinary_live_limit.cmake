# Each graph has a separate producer, generated image and fixed runtime oracle.
function(xr_add_ordinary_live_limit_graph suffix fixture modules functions)
    set(_oll_dir "${CMAKE_BINARY_DIR}/generated/ordinary_live_limit_${suffix}")
    set(_oll_c "${_oll_dir}/ordinary_live_limit.c")
    set(_oll_h "${_oll_dir}/ordinary_live_limit_generated.h")
    set(_oll_producer "test_xir_ordinary_live_limit_${suffix}_producer")

    set(_oll_definitions OLL_MODULES=${modules}u OLL_FUNCTIONS=${functions}u
        OLL_FIXTURE_DIRECTORY="${CMAKE_CURRENT_LIST_DIR}/fixtures/${fixture}")
    if(suffix STREQUAL "ready")
        list(APPEND _oll_definitions OLL_READY_GRAPH=1)
    endif()

    file(GLOB _oll_inputs CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/fixtures/${fixture}/*.xr")

    foreach(_oll_kind native mixed)
        set(_oll_target "test_xir_ordinary_live_limit_${suffix}_${_oll_kind}")

    endforeach()
    foreach(_oll_mode source vm native native-vm vm-native)
        if(_oll_mode STREQUAL "native")
            set(_oll_target "test_xir_ordinary_live_limit_${suffix}_native")
        else()
            set(_oll_target "test_xir_ordinary_live_limit_${suffix}_mixed")
        endif()
        string(REPLACE "-" "_" _oll_name "${_oll_mode}")

    endforeach()
endfunction()
# The independent READY graph does not replace the original 25-module graph.
xr_add_ordinary_live_limit_graph(ready ordinary_live_limit_ready 18 37)
