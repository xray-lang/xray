# A trusted static registry is derived from the actual compiled image object.
set(XIR_NATIVE_CACHE_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/native-cache)
set(XIR_NATIVE_CACHE_C ${XIR_NATIVE_CACHE_DIR}/io-output.c)
set(XIR_NATIVE_CACHE_CHECKED ${XIR_NATIVE_CACHE_DIR}/io-output.chk)
set(XIR_NATIVE_CACHE_PAIR ${XIR_NATIVE_CACHE_DIR}/pair.json)
set(XIR_NATIVE_CACHE_REGISTRY ${XIR_NATIVE_CACHE_DIR}/native-cache-registry.c)
add_executable(xir_native_cache_pairgen ${CMAKE_CURRENT_LIST_DIR}/xxir_native_cache_pairgen.c)
target_link_libraries(xir_native_cache_pairgen PRIVATE xray_xir_source xray_xir_cgen)
target_include_directories(xir_native_cache_pairgen PRIVATE ${XRAY_COMMON_INCLUDES})
add_custom_command(OUTPUT ${XIR_NATIVE_CACHE_C} ${XIR_NATIVE_CACHE_CHECKED} ${XIR_NATIVE_CACHE_PAIR}
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/gen_xir_native_cache_pair.py
        --root ${PROJECT_SOURCE_DIR} --producer $<TARGET_FILE:xir_native_cache_pairgen>
        --directory ${XIR_NATIVE_CACHE_DIR}
    DEPENDS xir_native_cache_pairgen ${PROJECT_SOURCE_DIR}/scripts/gen_xir_native_cache_pair.py
        ${PROJECT_SOURCE_DIR}/stdlib/io/output.xr
        ${CMAKE_CURRENT_LIST_DIR}/xxir_native_cache_output.xr
    VERBATIM)
add_library(xir_native_cache_image OBJECT ${XIR_NATIVE_CACHE_C})
target_include_directories(xir_native_cache_image PRIVATE ${XRAY_COMMON_INCLUDES})
target_link_libraries(xir_native_cache_image PRIVATE xray_xir_scalar)
if(WIN32)
    set(XIR_NATIVE_CACHE_OBJECT_FORMAT COFF)
else()
    set(XIR_NATIVE_CACHE_OBJECT_FORMAT ${CMAKE_EXECUTABLE_FORMAT})
endif()
set(XIR_NATIVE_CACHE_VARIANT "triple=${XRAY_HOST_TARGET_ABI}|compiler-target=${CMAKE_C_COMPILER_TARGET}|arch=${CMAKE_SYSTEM_PROCESSOR}|os=${CMAKE_SYSTEM_NAME}|format=${XIR_NATIVE_CACHE_OBJECT_FORMAT}|endian=${CMAKE_C_BYTE_ORDER}|pointer=${CMAKE_SIZEOF_VOID_P}|cc=cdecl|provider=${CMAKE_C_COMPILER_ID}-${CMAKE_C_COMPILER_VERSION}|profile=${CMAKE_BUILD_TYPE}|crt=${CMAKE_MSVC_RUNTIME_LIBRARY}|release=${CMAKE_C_FLAGS_RELEASE}|debug=${CMAKE_C_FLAGS_DEBUG}|flags=${CMAKE_C_FLAGS}|asan=${ENABLE_ASAN}|ubsan=${ENABLE_UBSAN}")
file(MAKE_DIRECTORY ${XIR_NATIVE_CACHE_DIR})
execute_process(COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/gen_xir_runtime_sdk_recipe.py
    --root ${PROJECT_SOURCE_DIR} --output ${XIR_NATIVE_CACHE_DIR}/sdk-recipe.inc
    --record ${XIR_NATIVE_CACHE_DIR}/sdk-recipe.json
    RESULT_VARIABLE XIR_NATIVE_CACHE_RECIPE_STATUS)
if(NOT XIR_NATIVE_CACHE_RECIPE_STATUS EQUAL 0)
    message(FATAL_ERROR "Cannot derive the native cache runtime input closure")
endif()
file(READ ${XIR_NATIVE_CACHE_DIR}/sdk-recipe.json XIR_NATIVE_CACHE_SDK_JSON)
string(JSON XIR_NATIVE_CACHE_SDK_COUNT LENGTH "${XIR_NATIVE_CACHE_SDK_JSON}" files)
math(EXPR XIR_NATIVE_CACHE_SDK_LAST "${XIR_NATIVE_CACHE_SDK_COUNT}-1")
set(XIR_NATIVE_CACHE_SDK_INPUTS)
foreach(index RANGE 0 ${XIR_NATIVE_CACHE_SDK_LAST})
    string(JSON kind GET "${XIR_NATIVE_CACHE_SDK_JSON}" files ${index} kind)
    string(JSON path GET "${XIR_NATIVE_CACHE_SDK_JSON}" files ${index} path)
    if(NOT kind EQUAL 5)
        list(APPEND XIR_NATIVE_CACHE_SDK_INPUTS ${PROJECT_SOURCE_DIR}/${path})
    endif()
endforeach()
add_custom_command(OUTPUT ${XIR_NATIVE_CACHE_REGISTRY}
    COMMAND ${XRAY_PYTHON} ${PROJECT_SOURCE_DIR}/scripts/gen_xir_native_cache_pair.py
        --root ${PROJECT_SOURCE_DIR} --directory ${XIR_NATIVE_CACHE_DIR}
        --object $<TARGET_OBJECTS:xir_native_cache_image>
        --compiler ${CMAKE_C_COMPILER} --variant ${XIR_NATIVE_CACHE_VARIANT}
    DEPENDS xir_native_cache_image ${XIR_NATIVE_CACHE_PAIR} ${XIR_NATIVE_CACHE_CHECKED}
        ${XIR_NATIVE_CACHE_SDK_INPUTS}
        ${PROJECT_SOURCE_DIR}/scripts/gen_xir_native_cache_pair.py
        ${PROJECT_SOURCE_DIR}/scripts/gen_xir_runtime_sdk_recipe.py
    VERBATIM)
add_library(xray_xir_native_cache STATIC xxir_native_cache.c ${XIR_NATIVE_CACHE_REGISTRY}
    $<TARGET_OBJECTS:xir_native_cache_image>)
target_link_libraries(xray_xir_native_cache PUBLIC xray_xir xray_xir_scalar)
target_include_directories(xray_xir_native_cache PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(xray_xir_vm PUBLIC xray_xir_native_cache)
foreach(cache_target xir_native_cache_pairgen xir_native_cache_image xray_xir_native_cache)
    if(MSVC)
        target_compile_options(${cache_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${cache_target} PRIVATE -Wall -Wextra -Werror)
    endif()
endforeach()
