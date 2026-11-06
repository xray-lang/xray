# Keep the retired registry controls independent of the legacy Core aggregate.

if(WIN32)

else()
    find_package(Threads REQUIRED)

endif()

foreach(builder_target
                       )
    target_include_directories(${builder_target} PRIVATE
        ${XRAY_COMMON_INCLUDES}
        ${CMAKE_SOURCE_DIR}/tests/unit
        ${CMAKE_BINARY_DIR}/generated)
    set_target_properties(${builder_target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(${builder_target} PRIVATE /utf-8 /Gy /Gw)
        if(CMAKE_C_COMPILER_ID STREQUAL "MSVC")
            target_compile_options(${builder_target} PRIVATE /experimental:c11atomics)
        endif()
    else()
        target_compile_options(${builder_target} PRIVATE -ffunction-sections -fdata-sections)
    endif()
endforeach()
foreach(builder_target  )
    if(MSVC)
        target_compile_options(${builder_target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${builder_target} PRIVATE -Wall -Wextra -Werror -pedantic)
    endif()
endforeach()
