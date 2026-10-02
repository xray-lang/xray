# The XIR compiler core is independent of legacy executors and stdlib bootstrap.
include("${CMAKE_CURRENT_LIST_DIR}/../toolchain/xcompiler_producer.cmake")
add_library(xray_xir_admission STATIC xxir.c xxir_program_match.c xxir_verify.c xxir_defaults.c xxir_effects.c xxir_layout.c xxir_checked.c xxir_generic.c ../base/xsha256.c)
add_library(xray_xir STATIC xxir_specialize.c)
add_library(xray_xir_declarations STATIC xxir_declarations.c xxir_types.c xxir_constraints.c xxir_constraint_proof.c xxir_interface.c xxir_interface_members.c xxir_implementation.c xxir_implementation_verify.c xxir_type_layout.c ../shared/xnative_declaration.c)
add_dependencies(xray_xir_declarations gen-native-type-defs)
target_include_directories(xray_xir_declarations PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(xray_xir_admission PUBLIC xray_xir_declarations)
target_include_directories(xray_xir_admission PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(xray_xir PUBLIC xray_xir_admission)
target_include_directories(xray_xir PUBLIC ${PROJECT_SOURCE_DIR}/src)
add_library(xray_xir_scalar STATIC xxir_scalar.c xxir_float.c xxir_value.c xxir_type_arena.c xxir_call.c xxir_program.c xxir_instance.c xxir_output.c)
target_include_directories(xray_xir_scalar PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(xray_xir_scalar PUBLIC xray_xir_admission)
add_library(xray_xir_vm STATIC xxir_vm.c)
target_link_libraries(xray_xir_vm PUBLIC xray_xir xray_xir_scalar)
add_library(xray_xir_cgen STATIC xxir_emit_c.c ../aot/xi_cgen_verify_output.c)
target_link_libraries(xray_xir_cgen PUBLIC xray_xir)
add_library(xray_xir_source STATIC xxir_source.c xxir_source_query.c xxir_type_inference.c xxir_library_catalog.c)
target_sources(xray_xir_source PRIVATE
    ../module/xmodule_graph.c ../module/xmodule_resolver.c ../module/xmodule_identity.c
    ../module/xmodule_identity_view.c ../module/xmodule_fingerprint.c
    ../module/xdeclaration_manifest.c ../module/xdeclaration_load.c
    ../module/xlockfile.c ../module/xsemver.c ../module/stdlib_embedded.c
    ../base/xhashmap.c ../base/xfileio.c ../base/xio_policy.c ../base/xtoml.c
    ../shared/xr_core_intrinsic_registry.c)
if(WIN32)
    target_sources(xray_xir_source PRIVATE ../os/win/fs_win.c ../os/win/dir_win.c ../os/win/file_read_win.c ../os/win/proc_win.c)
else()
    target_sources(xray_xir_source PRIVATE ../os/unix/fs_unix.c ../os/unix/dir_unix.c ../os/unix/file_read_unix.c ../os/unix/proc_unix.c)
endif()
add_dependencies(xray_xir_source gen-stdlib-embedded-sources)
target_include_directories(xray_xir_source PRIVATE "${CMAKE_BINARY_DIR}/generated")
set(XIR_CORE_SOURCE ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_core_declarations_source.inc.c)
add_custom_command(OUTPUT ${XIR_CORE_SOURCE}
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/embed_xir_core_source.py
        ${CMAKE_CURRENT_SOURCE_DIR}/xcore_declarations.xr --output ${XIR_CORE_SOURCE}
    DEPENDS ${PROJECT_SOURCE_DIR}/scripts/embed_xir_core_source.py ${CMAKE_CURRENT_SOURCE_DIR}/xcore_declarations.xr
    VERBATIM)
add_custom_target(gen-xir-core-source DEPENDS ${XIR_CORE_SOURCE})
add_dependencies(xray_xir_source gen-xir-core-source)
target_include_directories(xray_xir_source PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/generated)
target_include_directories(xray_xir_source PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(xray_xir_source PUBLIC xray_xir xray_xir_scalar xray_compiler_parser)
add_library(xray_xir_source_product STATIC ../program/xr_xir_source_product.c)
target_link_libraries(xray_xir_source_product PUBLIC xray_xir_source xray_xir_vm xray_xir_cgen)
foreach(target xray_xir xray_xir_admission xray_xir_declarations xray_xir_scalar xray_xir_vm xray_xir_cgen xray_xir_source xray_xir_source_product)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
