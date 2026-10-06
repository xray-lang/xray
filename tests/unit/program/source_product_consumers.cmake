# Retiring consumers retain exact source inputs and independent result goldens.
set(product_consumer_source "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/test_source_product_consumer.c")
set(product_consumer_fixture_root "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/fixtures")
get_filename_component(product_consumer_fixture_root "${product_consumer_fixture_root}" REALPATH)
set(product_consumer_fault_runner "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_faults.py")
set(product_consumer_main_template "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_consumer_main.c.in")
add_library(source_product_consumer_driver OBJECT "${product_consumer_source}")
target_link_libraries(source_product_consumer_driver PUBLIC xray_xir_source_product)
target_include_directories(source_product_consumer_driver PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers")
set_target_properties(source_product_consumer_driver PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(source_product_consumer_driver PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(source_product_consumer_driver PRIVATE -Wall -Wextra -Werror)
endif()
add_executable(test_source_product_probe "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/source_product_consumer_probe.c")
target_link_libraries(test_source_product_probe PRIVATE source_product_consumer_driver)
set_target_properties(test_source_product_probe PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_probe PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_probe PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case IN ITEMS static_methods array_places generics callables class_identity struct_string bool_conditions single_module text_program)
    if(case STREQUAL "callables")
        set(expected 48)
    elseif(case STREQUAL "text_program")
        set(expected 7)
    else()
        set(expected 42)
    endif()
    set(producer "test_source_product_${case}")
    set(native "${producer}_native")
    set(generated "${CMAKE_BINARY_DIR}/generated/source_product_${case}.c")
    set(consumer_has_native 0)
    set(main "${CMAKE_CURRENT_BINARY_DIR}/source_product_${case}_main.c")
    configure_file("${product_consumer_main_template}" "${main}" @ONLY)
    add_executable(${producer} "${main}")
    target_link_libraries(${producer} PRIVATE source_product_consumer_driver)
    file(GLOB product_consumer_case_sources "${product_consumer_fixture_root}/${case}/*.xr")
    add_custom_command(OUTPUT "${generated}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/generated"
        COMMAND $<TARGET_FILE:${producer}> 0 "${generated}"
        DEPENDS ${producer} ${product_consumer_case_sources}
        VERBATIM)
    set(consumer_has_native 1)
    set(native_main "${CMAKE_CURRENT_BINARY_DIR}/source_product_${case}_native_main.c")
    configure_file("${product_consumer_main_template}" "${native_main}" @ONLY)
    add_executable(${native} "${native_main}" "${generated}")
    target_link_libraries(${native} PRIVATE source_product_consumer_driver)
    foreach(target IN ITEMS ${producer} ${native})
        set_target_properties(${target} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
        if(MSVC)
            target_compile_options(${target} PRIVATE /utf-8 /W4 /WX)
        else()
            target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
        endif()
    endforeach()
    add_test(NAME ${producer}_vm COMMAND ${producer} 0)
    add_test(NAME ${producer}_native COMMAND ${native} 1)
    add_test(NAME ${producer}_mixed_even COMMAND ${native} 2)
    add_test(NAME ${producer}_mixed_odd COMMAND ${native} 3)
    set_tests_properties(${producer}_vm ${producer}_native ${producer}_mixed_even
        ${producer}_mixed_odd PROPERTIES TIMEOUT 120
        LABELS "unit;xir;source-product;program-consumer;ownership;execution")
    foreach(mode IN ITEMS 0 1 2 3)
        if(mode EQUAL 0)
            set(executable ${producer})
        else()
            set(executable ${native})
        endif()
        foreach(check IN ITEMS axes cancel runtime)
            add_test(NAME ${producer}_${check}_${mode} COMMAND ${executable} ${mode} --${check})
            set_tests_properties(${producer}_${check}_${mode} PROPERTIES TIMEOUT 120
                LABELS "unit;xir;source-product;program-consumer;ownership;${check}")
        endforeach()
        add_test(NAME ${producer}_compiler_${mode}
            COMMAND ${XRAY_PYTHON} -X utf8 "${product_consumer_fault_runner}"
                --binary $<TARGET_FILE:${executable}> --mode ${mode} --jobs 8
                --evidence "${CMAKE_BINARY_DIR}/consumer-fault-evidence/${case}-${mode}")
        set_tests_properties(${producer}_compiler_${mode} PROPERTIES TIMEOUT 600 PROCESSORS 8
            LABELS "unit;xir;source-product;program-consumer;ownership;compiler-faults")
        if(case STREQUAL "text_program")
            add_test(NAME ${producer}_output_status_${mode} COMMAND ${executable} ${mode} --output-status)
            set_tests_properties(${producer}_output_status_${mode} PROPERTIES TIMEOUT 120
                LABELS "unit;xir;source-product;program-consumer;ownership;output")
        endif()
    endforeach()
endforeach()
add_executable(test_source_product_detachment
    "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/test_source_product_detachment.c")
target_link_libraries(test_source_product_detachment PRIVATE xray_xir_source_product)
target_compile_definitions(test_source_product_detachment PRIVATE
    XR_DETACHMENT_ROOT="${product_consumer_fixture_root}"
    XR_DETACHMENT_SCRATCH="${CMAKE_BINARY_DIR}/consumer-detachment-inputs")
set_target_properties(test_source_product_detachment PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_detachment PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_detachment PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case IN ITEMS generics callables single_module text_program)
    add_test(NAME test_source_product_detachment_${case}
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_detachment.py"
            --binary $<TARGET_FILE:test_source_product_detachment> --case ${case}
            --scratch "${CMAKE_BINARY_DIR}/consumer-detachment-inputs")
    set_tests_properties(test_source_product_detachment_${case} PROPERTIES TIMEOUT 120
        LABELS "unit;xir;source-product;program-consumer;ownership;detachment")
    add_test(NAME test_source_product_detachment_${case}_axes
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_detachment.py"
            --binary $<TARGET_FILE:test_source_product_detachment> --case ${case}
            --scratch "${CMAKE_BINARY_DIR}/consumer-detachment-inputs" --check axes)
    set_tests_properties(test_source_product_detachment_${case}_axes PROPERTIES TIMEOUT 120
        LABELS "unit;xir;source-product;program-consumer;ownership;detachment;axes")
    add_test(NAME test_source_product_detachment_${case}_compiler
        COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_detachment_faults.py"
            --binary $<TARGET_FILE:test_source_product_detachment> --case ${case}
            --scratch "${CMAKE_BINARY_DIR}/consumer-detachment-inputs" --jobs 8
            --evidence "${CMAKE_BINARY_DIR}/consumer-detachment-fault-evidence/${case}")
    set_tests_properties(test_source_product_detachment_${case}_compiler PROPERTIES TIMEOUT 600 PROCESSORS 8
        LABELS "unit;xir;source-product;program-consumer;ownership;detachment;compiler-faults")
endforeach()
add_executable(test_source_product_rejections
    "${CMAKE_CURRENT_LIST_DIR}/../xir/product_consumers/test_source_product_rejections.c")
target_link_libraries(test_source_product_rejections PRIVATE xray_xir_source_product)
target_compile_definitions(test_source_product_rejections PRIVATE XR_REJECTION_ROOT="${product_consumer_fixture_root}")
set_target_properties(test_source_product_rejections PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
if(MSVC)
    target_compile_options(test_source_product_rejections PRIVATE /utf-8 /W4 /WX)
else()
    target_compile_options(test_source_product_rejections PRIVATE -Wall -Wextra -Werror)
endif()
foreach(case IN ITEMS bare_nullable missing_name)
    set(test "test_source_product_rejections_${case}")
    add_test(NAME ${test} COMMAND test_source_product_rejections ${case})
    add_test(NAME ${test}_axes COMMAND test_source_product_rejections ${case} --axes)
    add_test(NAME ${test}_compiler COMMAND test_source_product_rejections ${case} --compiler)
    set_tests_properties(${test} ${test}_axes PROPERTIES TIMEOUT 120
        LABELS "unit;xir;source-product;program-consumer;ownership;rejection")
    set_tests_properties(${test}_compiler PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE
        LABELS "unit;xir;source-product;program-consumer;ownership;compiler-faults")
endforeach()
add_test(NAME source_product_consumer_inventory
    COMMAND ${XRAY_PYTHON} -X utf8 "${CMAKE_SOURCE_DIR}/scripts/source_product_consumer_inventory.py" check)
set_tests_properties(source_product_consumer_inventory PROPERTIES TIMEOUT 30
    LABELS "unit;xir;source-product;program-consumer;inventory")
