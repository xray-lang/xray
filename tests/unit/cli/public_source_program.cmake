if(NOT WIN32)
    return()
endif()
add_test(NAME public_source_program COMMAND ${XRAY_PYTHON} -X utf8
    ${CMAKE_CURRENT_LIST_DIR}/test_public_source_program.py
    --root ${PROJECT_SOURCE_DIR} --cli $<TARGET_FILE:xray>
    --output ${CMAKE_CURRENT_BINARY_DIR}/public-source-program
    --sdk ${CMAKE_BINARY_DIR}/xir-runtime-sdk
    --cc ${XIR_INVOCATION_MSVC} --linker ${XIR_INVOCATION_LINKER})
set_tests_properties(public_source_program PROPERTIES TIMEOUT 180 COST 4
    LABELS "unit;cli;xir;source;vm;generated-c;native;ownership")
