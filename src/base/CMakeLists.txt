# Compiler resource ownership has no executor or stdlib dependency.
add_library(xray_compile_resources STATIC xcompile_resources.c)
target_include_directories(xray_compile_resources PUBLIC ${PROJECT_SOURCE_DIR}/src)
set_target_properties(xray_compile_resources PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(xray_compile_resources PRIVATE /W4 /WX)
else()
    target_compile_options(xray_compile_resources PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
