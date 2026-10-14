# The Catalog producer contains no CGen, VM or native code dependency.
set(XIR_STDLIB_CHECKED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/stdlib-checked)
set(XIR_STDLIB_CHECKED ${XIR_STDLIB_CHECKED_DIR}/io-output.chk)
set(XIR_STDLIB_DATA ${XIR_STDLIB_CHECKED_DIR}/stdlib-checked-data.c)
set(XIR_STDLIB_MANIFEST ${XIR_STDLIB_CHECKED_DIR}/stdlib-manifest.json)
set(XRAY_STDLIB_CHECKED_MANIFEST ${XIR_STDLIB_MANIFEST} PARENT_SCOPE)
add_executable(xir_stdlib_checkedgen ${CMAKE_CURRENT_LIST_DIR}/xxir_stdlib_checkedgen.c)
target_link_libraries(xir_stdlib_checkedgen PRIVATE xray_xir_source)
target_include_directories(xir_stdlib_checkedgen PRIVATE ${XRAY_COMMON_INCLUDES})
add_custom_command(OUTPUT ${XIR_STDLIB_CHECKED} ${XIR_STDLIB_DATA} ${XIR_STDLIB_MANIFEST}
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/gen_xir_stdlib_checked.py
        --root ${PROJECT_SOURCE_DIR} --producer $<TARGET_FILE:xir_stdlib_checkedgen>
        --directory ${XIR_STDLIB_CHECKED_DIR}
    DEPENDS xir_stdlib_checkedgen ${PROJECT_SOURCE_DIR}/scripts/gen_xir_stdlib_checked.py
        ${PROJECT_SOURCE_DIR}/stdlib/io/output.xr
    VERBATIM)
add_library(xray_xir_stdlib_catalog STATIC xxir_stdlib_catalog.c ${XIR_STDLIB_DATA})
target_link_libraries(xray_xir_stdlib_catalog PUBLIC xray_xir_source)
target_include_directories(xray_xir_stdlib_catalog PUBLIC ${PROJECT_SOURCE_DIR}/src)
install(FILES ${XIR_STDLIB_CHECKED}
    DESTINATION lib/xray/stdlib-xir/io/output COMPONENT XrayCore)
foreach(stdlib_target xir_stdlib_checkedgen xray_xir_stdlib_catalog)
    if(MSVC)
        target_compile_options(${stdlib_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${stdlib_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
