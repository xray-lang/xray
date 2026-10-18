if(WIN32)
    # Exercise the actual MD source archive closure with the original exhaustive facade matrix.
    add_executable(test_source_sdk_faults
        "${PROJECT_SOURCE_DIR}/tests/unit/api/source_public/test_source_public_faults.c")
    target_link_libraries(test_source_sdk_faults PRIVATE ${XRAY_SOURCE_SDK_ARCHIVE_TARGETS})
    target_include_directories(test_source_sdk_faults PRIVATE "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests/unit")
    target_compile_definitions(test_source_sdk_faults PRIVATE XR_PUBLIC_STDLIB="${PROJECT_SOURCE_DIR}/stdlib"
        XR_PUBLIC_REPL_FIXTURE="${PROJECT_SOURCE_DIR}/tests/unit/api/source_public_repl_rooted"
        XR_PUBLIC_REPL_FILE_FIXTURE="${PROJECT_SOURCE_DIR}/tests/unit/api/source_public_repl_file")
    set_target_properties(test_source_sdk_faults PROPERTIES MSVC_RUNTIME_LIBRARY MultiThreadedDLL
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(test_source_sdk_faults PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(test_source_sdk_faults PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
    add_test(NAME source_sdk_faults COMMAND test_source_sdk_faults)
    set_tests_properties(source_sdk_faults PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE COST 100
        LABELS "unit;api;source;sdk;ownership;compiler-faults;runtime-faults")
    add_test(NAME source_sdk_repl_faults COMMAND test_source_sdk_faults --repl)
    set_tests_properties(source_sdk_repl_faults PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE COST 120
        LABELS "unit;api;source;sdk;ownership;repl;compiler-faults;runtime-faults")
    add_test(NAME source_sdk_repl_inspect_faults COMMAND test_source_sdk_faults --repl-inspect)
    set_tests_properties(source_sdk_repl_inspect_faults PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE COST 120
        LABELS "unit;api;source;sdk;ownership;repl;inspection;compiler-faults;runtime-faults")
    add_test(NAME source_sdk_repl_inspect_probe COMMAND test_source_sdk_faults --repl-inspect-probe)
    add_test(NAME source_sdk_repl_inspect_budget COMMAND test_source_sdk_faults --repl-inspect-budget)
    add_test(NAME source_sdk_repl_inspect_samples COMMAND test_source_sdk_faults --repl-inspect-samples)
    set_tests_properties(source_sdk_repl_inspect_probe source_sdk_repl_inspect_budget source_sdk_repl_inspect_samples
        PROPERTIES TIMEOUT 120 LABELS "unit;api;source;sdk;ownership;repl;inspection;budget")
    foreach(input_kind IN ITEMS rooted file array write)
        add_test(NAME source_sdk_repl_${input_kind}_faults COMMAND test_source_sdk_faults --repl-${input_kind})
        set_tests_properties(source_sdk_repl_${input_kind}_faults PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE COST 120
            LABELS "unit;api;source;sdk;ownership;repl;compiler-faults;runtime-faults;${input_kind}")
        foreach(mode IN ITEMS probe budget samples)
            add_test(NAME source_sdk_repl_${input_kind}_${mode} COMMAND test_source_sdk_faults --repl-${input_kind}-${mode})
            set_tests_properties(source_sdk_repl_${input_kind}_${mode} PROPERTIES TIMEOUT 120
                LABELS "unit;api;source;sdk;ownership;repl;${input_kind};budget")
        endforeach()
    endforeach()
endif()
if(WIN32 AND NOT ENABLE_ASAN AND NOT ENABLE_SANITIZERS AND NOT ENABLE_UBSAN
    AND NOT ENABLE_TSAN AND NOT ENABLE_MSAN)
    add_test(NAME source_sdk_installed COMMAND ${XRAY_PYTHON} -B
        "${CMAKE_CURRENT_LIST_DIR}/source_sdk_install.py"
        --build "${CMAKE_BINARY_DIR}" --compiler "${CMAKE_C_COMPILER}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/source-sdk-installed")
    set_tests_properties(source_sdk_installed PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE COST 30 LABELS "unit;api;source;sdk;install;ownership")
    set(source_sdk_owned_secondary)
    if(CMAKE_C_COMPILER_ID STREQUAL "Clang" AND XIR_INVOCATION_MSVC)
        list(APPEND source_sdk_owned_secondary --secondary-compiler "${XIR_INVOCATION_MSVC}")
    elseif(XIR_CLI_CLANG)
        list(APPEND source_sdk_owned_secondary --secondary-compiler "${XIR_CLI_CLANG}")
    endif()
    add_test(NAME source_sdk_owned_installed COMMAND ${XRAY_PYTHON} -B
        "${CMAKE_CURRENT_LIST_DIR}/source_sdk_owned.py"
        --build "${CMAKE_BINARY_DIR}" --compiler "${CMAKE_C_COMPILER}"
        --output "${CMAKE_CURRENT_BINARY_DIR}/source-sdk-owned" ${source_sdk_owned_secondary})
    set_tests_properties(source_sdk_owned_installed PROPERTIES
        TIMEOUT 300 RUN_SERIAL TRUE COST 30 LABELS "unit;api;source;sdk;install;ownership;values")

    find_program(SOURCE_SDK_DUMPBIN NAMES dumpbin REQUIRED)
    find_program(SOURCE_SDK_CLANG_CL NAMES clang-cl HINTS "C:/Program Files/LLVM/bin" REQUIRED)
    get_filename_component(source_clang_bin "${SOURCE_SDK_CLANG_CL}" DIRECTORY)
    file(GLOB source_asan_candidates "${source_clang_bin}/../lib/clang/*/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
    list(SORT source_asan_candidates COMPARE NATURAL ORDER DESCENDING)
    list(LENGTH source_asan_candidates source_asan_count)
    if(source_asan_count EQUAL 0)
        message(FATAL_ERROR "The installed Source ASan client gate requires the actual Clang ASan runtime")
    endif()
    list(GET source_asan_candidates 0 source_asan_dll)
    get_filename_component(source_asan_runtime "${source_asan_dll}" DIRECTORY)
    add_custom_target(source-sdk-public-consumers DEPENDS source-sdk-bundle xir-runtime-sdk-bundle)
    foreach(mode IN ITEMS normal asan)
        set(source_client_args)
        set(source_client_compiler "${CMAKE_C_COMPILER}")
        if(mode STREQUAL "asan")
            set(source_client_compiler "${SOURCE_SDK_CLANG_CL}")
            list(APPEND source_client_args --asan-runtime "${source_asan_runtime}")
        endif()
        add_test(NAME source_sdk_public_${mode}_clients COMMAND ${XRAY_PYTHON} -X utf8 -B
            "${CMAKE_CURRENT_LIST_DIR}/source_sdk_smoke.py" --build "${CMAKE_BINARY_DIR}"
            --compiler "${source_client_compiler}" --dumpbin "${SOURCE_SDK_DUMPBIN}"
            --output "${CMAKE_CURRENT_BINARY_DIR}/source-sdk-public-${mode}" ${source_client_args})
        set_tests_properties(source_sdk_public_${mode}_clients PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE COST 10
            LABELS "unit;api;source;sdk;install;ownership;public-client")
    endforeach()
    if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        add_test(NAME cli_toolchain_probe_repaired COMMAND ${XRAY_PYTHON} -X utf8 -B
            "${PROJECT_SOURCE_DIR}/tests/unit/cli/test_cli_toolchain_probe.py"
            $<TARGET_FILE:xray> "${CMAKE_C_COMPILER}"
            "${CMAKE_CURRENT_BINARY_DIR}/toolchain-probe-repaired")
        set_tests_properties(cli_toolchain_probe_repaired PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE COST 10
            LABELS "unit;cli;source;sdk;native;probe")
    endif()
endif()
