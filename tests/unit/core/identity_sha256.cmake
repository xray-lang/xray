# Hashes come from the real admission library; no copied implementation or legacy core.
add_executable(test_identity_sha256 "${CMAKE_CURRENT_LIST_DIR}/test_identity_sha256.c")
target_link_libraries(test_identity_sha256 PRIVATE xray_xir_admission)
set_target_properties(test_identity_sha256 PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(test_identity_sha256 PRIVATE /W4 /WX /utf-8)
    target_compile_definitions(test_identity_sha256 PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(test_identity_sha256 PRIVATE -Wall -Wextra -Werror -pedantic)
endif()
set(IDENTITY_SHA_ASAN 0)
set(IDENTITY_SHA_UBSAN 0)
if(ENABLE_ASAN OR ENABLE_SANITIZERS)
    set(IDENTITY_SHA_ASAN 1)
endif()
if(ENABLE_UBSAN OR ENABLE_SANITIZERS)
    set(IDENTITY_SHA_UBSAN 1)
endif()
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME test_identity_sha256
    COMMAND "${Python3_EXECUTABLE}" -X utf8 "${CMAKE_CURRENT_LIST_DIR}/test_identity_sha256.py"
        --executable $<TARGET_FILE:test_identity_sha256>
        --debug $<IF:$<CONFIG:Debug>,1,0> --asan ${IDENTITY_SHA_ASAN} --ubsan ${IDENTITY_SHA_UBSAN})
set_tests_properties(test_identity_sha256 PROPERTIES LABELS "unit;identity;sha256;regression" TIMEOUT 120)
