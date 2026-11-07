# One source Checked packet and generated C feed VM/native/mixed class identity tests.
set(XIR_CLASS_CHECKED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_class_identity.xrc)
set(XIR_CLASS_GENERATED ${CMAKE_CURRENT_BINARY_DIR}/generated/xir_class_identity.c)








foreach(class_mode IN ITEMS native mixed)
    add_executable(test_xir_class_${class_mode} xir/test_xir_class_execution.c ${XIR_CLASS_GENERATED})
    target_link_libraries(test_xir_class_${class_mode} PRIVATE )
    if(class_mode STREQUAL "mixed")
        target_compile_definitions(test_xir_class_${class_mode} PRIVATE XR_CLASS_MIXED=1)
    else()
        target_compile_definitions(test_xir_class_${class_mode} PRIVATE XR_CLASS_MIXED=0)
    endif()
endforeach()
