# Optimize the fixed SHA computation without changing Debug assertions,
# instrumentation, symbols, or the surrounding compiler/resource owners.
function(xray_optimize_identity_kernel directory)
    if(MSVC)
        # ASan configuration already removes incompatible /RTC defaults.
        # Ordinary MSVC Debug retains those checks and its original flags.
        if(NOT ENABLE_ASAN)
            return()
        endif()
        set(_xray_identity_options "$<$<CONFIG:Debug>:/O2;/Ob2;/Oy->")
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            list(APPEND _xray_identity_options "$<$<CONFIG:Debug>:/clang:-fno-omit-frame-pointer>")
        endif()
    elseif(ENABLE_ASAN OR ENABLE_UBSAN OR ENABLE_SANITIZERS OR ENABLE_TSAN OR ENABLE_MSAN)
        set(_xray_identity_options "$<$<CONFIG:Debug>:-O2;-fno-omit-frame-pointer>")
    else()
        return()
    endif()
    get_filename_component(_xray_identity_source
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../src/base/xsha256.c" ABSOLUTE)
    set_property(SOURCE "${_xray_identity_source}" DIRECTORY "${directory}"
        APPEND PROPERTY COMPILE_OPTIONS "${_xray_identity_options}")
    get_property(_xray_identity_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_xray_identity_child IN LISTS _xray_identity_children)
        xray_optimize_identity_kernel("${_xray_identity_child}")
    endforeach()
endfunction()
