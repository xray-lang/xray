# Verify that MSVC /showIncludes output remains parseable by Ninja.

foreach(_xray_required_variable IN ITEMS
        XRAY_BUILD_DIR XRAY_C_COMPILER XRAY_PROBE_TARGET XRAY_PROBE_HEADER
        XRAY_PROBE_OBJECT_DIR)
    if(NOT DEFINED ${_xray_required_variable} OR "${${_xray_required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${_xray_required_variable} is required")
    endif()
endforeach()

if(NOT EXISTS "${XRAY_PROBE_HEADER}")
    message(FATAL_ERROR "Generated transitive-header probe is missing: ${XRAY_PROBE_HEADER}")
endif()

set(_xray_rules "${XRAY_BUILD_DIR}/CMakeFiles/rules.ninja")
set(_xray_build "${XRAY_BUILD_DIR}/build.ninja")
if(NOT EXISTS "${_xray_rules}" OR NOT EXISTS "${_xray_build}")
    message(FATAL_ERROR "MSVC Ninja rules are missing from ${XRAY_BUILD_DIR}")
endif()

file(READ "${_xray_rules}" _xray_rules_text)
string(REGEX MATCHALL "msvc_deps_prefix = [^\r\n]*" _xray_prefixes "${_xray_rules_text}")
list(LENGTH _xray_prefixes _xray_prefix_count)
if(NOT _xray_prefix_count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one MSVC /showIncludes prefix, found ${_xray_prefix_count}")
endif()
list(GET _xray_prefixes 0 _xray_prefix)
file(WRITE "${XRAY_BUILD_DIR}/CMakeFiles/XrayShowIncludesRegression/rules_probe.h" "\n")
file(WRITE "${XRAY_BUILD_DIR}/CMakeFiles/XrayShowIncludesRegression/rules_probe.c"
    "#include \"rules_probe.h\"\nint main(void) { return 0; }\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "VSLANG=1033"
            "${XRAY_C_COMPILER}" /nologo /showIncludes /c rules_probe.c
    WORKING_DIRECTORY "${XRAY_BUILD_DIR}/CMakeFiles/XrayShowIncludesRegression"
    OUTPUT_VARIABLE _xray_showincludes_output
    ERROR_VARIABLE _xray_showincludes_error
    RESULT_VARIABLE _xray_showincludes_result
    ENCODING UTF-8
)
if(NOT _xray_showincludes_result EQUAL 0 OR
   NOT _xray_showincludes_output MATCHES
       "(^|\n)([^:\n][^:\n]+:[^:\n]*[^: \n][^: \n]:?[ \t]+)([A-Za-z]:\\\\|\\./|\\.\\\\|/)")
    message(FATAL_ERROR
        "Unable to determine the UTF-8 MSVC /showIncludes prefix:\n"
        "${_xray_showincludes_output}${_xray_showincludes_error}")
endif()
set(_xray_expected_prefix "msvc_deps_prefix = ${CMAKE_MATCH_2}")
if(NOT "${_xray_prefix}" STREQUAL "${_xray_expected_prefix}")
    message(FATAL_ERROR
        "MSVC Ninja dependency prefix differs from cl.exe output:\n"
        "rules.ninja: ${_xray_prefix}\n"
        "cl.exe:      ${_xray_expected_prefix}")
endif()

file(READ "${_xray_build}" _xray_build_text)
string(FIND "${_xray_build_text}" "VSLANG=1033" _xray_vslang_offset)
if(_xray_vslang_offset EQUAL -1)
    message(FATAL_ERROR "MSVC Ninja compile launchers must set VSLANG=1033")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${XRAY_BUILD_DIR}"
            --target "${XRAY_PROBE_TARGET}"
    RESULT_VARIABLE _xray_first_build_result
)
if(NOT _xray_first_build_result EQUAL 0)
    message(FATAL_ERROR "Unable to build the MSVC transitive-header probe")
endif()
file(GLOB_RECURSE _xray_probe_objects "${XRAY_PROBE_OBJECT_DIR}/*.obj")
list(LENGTH _xray_probe_objects _xray_probe_object_count)
if(NOT _xray_probe_object_count EQUAL 1)
    message(FATAL_ERROR "Expected one transitive-header probe object, found ${_xray_probe_object_count}")
endif()
list(GET _xray_probe_objects 0 _xray_probe_object)
file(TIMESTAMP "${_xray_probe_object}" _xray_before_timestamp "%s")
file(SHA256 "${_xray_probe_object}" _xray_before_hash)

file(READ "${XRAY_PROBE_HEADER}" _xray_probe_header_text)
if(_xray_probe_header_text MATCHES "VALUE 1")
    set(_xray_next_value 2)
else()
    set(_xray_next_value 1)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" -E sleep 2)
file(WRITE "${XRAY_PROBE_HEADER}"
    "#define XRAY_SHOWINCLUDES_REGRESSION_VALUE ${_xray_next_value}\n")
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${XRAY_BUILD_DIR}"
            --target "${XRAY_PROBE_TARGET}"
    RESULT_VARIABLE _xray_second_build_result
)
if(NOT _xray_second_build_result EQUAL 0)
    message(FATAL_ERROR "Unable to rebuild the MSVC transitive-header probe")
endif()
file(TIMESTAMP "${_xray_probe_object}" _xray_after_timestamp "%s")
file(SHA256 "${_xray_probe_object}" _xray_after_hash)
if(_xray_before_timestamp STREQUAL _xray_after_timestamp OR
   _xray_before_hash STREQUAL _xray_after_hash)
    message(FATAL_ERROR
        "Changing a transitive header did not rebuild its MSVC Ninja object")
endif()
