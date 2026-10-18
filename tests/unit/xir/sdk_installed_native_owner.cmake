if(NOT WIN32)
    return()
endif()
find_program(XIR_INSTALLED_SDK_CLANG NAMES clang-cl HINTS "C:/Program Files/LLVM/bin")
if(NOT XIR_INSTALLED_SDK_CLANG)
    set(XIR_INSTALLED_SDK_CLANG clang-cl)
endif()
find_program(XIR_INSTALLED_SDK_DUMPBIN NAMES dumpbin REQUIRED)
add_custom_target(sdk-installed-native-consumers
    DEPENDS xir-runtime-sdk-bundle test_xir_compile_emit_owner)
foreach(provider IN ITEMS msvc clang)
    if(provider STREQUAL "msvc")
        set(compiler "${XIR_INVOCATION_MSVC}")
    else()
        set(compiler "${XIR_INSTALLED_SDK_CLANG}")
    endif()
    add_test(NAME sdk_installed_native_${provider}
        COMMAND ${XRAY_PYTHON} -X utf8
            "${CMAKE_CURRENT_LIST_DIR}/sdk_installed_native_owner.py"
            --cmake "${CMAKE_COMMAND}" --ninja "${CMAKE_MAKE_PROGRAM}" --build "${CMAKE_BINARY_DIR}"
            --producer $<TARGET_FILE:test_xir_compile_emit_owner>
            --consumer-source "${CMAKE_CURRENT_LIST_DIR}/test_xir_compile_native_owner.c"
            --project "${CMAKE_CURRENT_LIST_DIR}/sdk_installed_native_owner"
            --compiler "${compiler}" --dumpbin "${XIR_INSTALLED_SDK_DUMPBIN}"
            --provider ${provider}
            --output "${CMAKE_CURRENT_BINARY_DIR}/sdk-installed-native-${provider}")
    set_tests_properties(sdk_installed_native_${provider} PROPERTIES
        TIMEOUT 180 COST 10 RUN_SERIAL TRUE
        LABELS "unit;xir;sdk;installed;generated-c;ownership")
endforeach()
