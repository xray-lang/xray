# Syntax, shared algorithms and host helpers have explicit narrow owners.
target_sources(test_xhash PRIVATE "${PROJECT_SOURCE_DIR}/src/base/xhash.c")
target_sources(test_log PRIVATE "${PROJECT_SOURCE_DIR}/src/base/xlog.c")
target_sources(test_config PRIVATE "${PROJECT_SOURCE_DIR}/src/base/xconfig.c")
target_include_directories(test_config PRIVATE "${PROJECT_BINARY_DIR}/generated")
target_sources(test_swar PRIVATE "${PROJECT_SOURCE_DIR}/src/base/xswar.c")
target_sources(test_intmap PRIVATE "${PROJECT_SOURCE_DIR}/src/base/xintmap.c")
target_sources(test_source_cache PRIVATE "${PROJECT_SOURCE_DIR}/src/base/xsource_cache.c")
target_sources(test_xml_node PRIVATE "${PROJECT_SOURCE_DIR}/src/base/xxml.c")
target_sources(test_crypto PRIVATE "${PROJECT_SOURCE_DIR}/src/shared/xr_crypto_core.c")
target_sources(test_unicode_grapheme_table PRIVATE
    "${PROJECT_SOURCE_DIR}/src/shared/xr_unicode_grapheme_data.c")
target_sources(test_unicode_grapheme_cursor PRIVATE
    "${PROJECT_SOURCE_DIR}/src/shared/xr_unicode_grapheme_data.c"
    "${PROJECT_SOURCE_DIR}/src/shared/xr_unicode_grapheme.c")

# These tests exercise the current module graph and its owned source inputs.
target_sources(test_resolver PRIVATE "${PROJECT_SOURCE_DIR}/src/module/xresolver.c")
foreach(target IN ITEMS test_resolver test_module_graph test_semver test_lockfile)
    target_link_libraries(${target} PRIVATE xray_xir_source)
endforeach()
target_link_libraries(test_project PRIVATE xray_source_compile)

# Real OS operations keep their existing platform implementations.
foreach(target IN ITEMS test_fs_atomic test_mutex test_numeric_core test_os_core)
    target_link_libraries(${target} PRIVATE xray_xir_source)
endforeach()
if(WIN32)
    target_sources(test_fs_atomic PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/temp_win.c")
    target_sources(test_os_core PRIVATE "${PROJECT_SOURCE_DIR}/src/os/win/proc_win.c"
        "${PROJECT_SOURCE_DIR}/src/os/win/pipe_win.c")
    target_link_libraries(test_mutex PRIVATE synchronization)
else()
    target_sources(test_fs_atomic PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/temp_unix.c")
    target_sources(test_os_core PRIVATE "${PROJECT_SOURCE_DIR}/src/os/unix/proc_unix.c"
        "${PROJECT_SOURCE_DIR}/src/os/unix/pipe_unix.c")
    foreach(target IN ITEMS test_xvalue test_math_core test_numeric_core test_compare_core
            test_truthy_core test_typed_ops test_mutex test_os_core)
        target_link_libraries(${target} PRIVATE m pthread)
    endforeach()
endif()
