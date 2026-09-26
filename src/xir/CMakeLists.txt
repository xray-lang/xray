# The XIR compiler core is independent of legacy executors and stdlib bootstrap.
add_library(xray_xir STATIC xxir.c xxir_verify.c)
target_include_directories(xray_xir PUBLIC ${PROJECT_SOURCE_DIR}/src)
if(MSVC)
    target_compile_options(xray_xir PRIVATE /W4 /WX)
else()
    target_compile_options(xray_xir PRIVATE -Wall -Wextra -Werror)
endif()
