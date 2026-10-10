# STATIC_NOT_RUN: caller-bound typed FI/axes CTest registration preparation.
# This file has no default include. Root must supply a fresh shared admission,
# four-material cohort, dedicated FI executable and caller-named INPUT record env.
# The outer admission sets/protects the actual postbuild INPUT record at runtime.
# Configure never embeds the not-yet-generated INPUT hash/size/mtime.
# The current Root public caller configuration is not delivered in this slice.
foreach(source_typed_fi_required IN ITEMS
        SOURCE_TYPED_LIBRARY_CURRENT_FI_COHORT_INPUT SOURCE_TYPED_LIBRARY_CURRENT_FI_COHORT_RECORD_ENV
        SOURCE_TYPED_LIBRARY_CURRENT_FI_GUARD_MODULE SOURCE_TYPED_LIBRARY_CURRENT_FI_GUARD_RECORD
        SOURCE_TYPED_LIBRARY_CURRENT_FI_EVIDENCE_ROOT SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT)
    if(NOT DEFINED ${source_typed_fi_required} OR "${${source_typed_fi_required}}" STREQUAL "")
        message(FATAL_ERROR "Typed FI/axes require explicit Root admission: ${source_typed_fi_required}")
    endif()
endforeach()
if(NOT "${SOURCE_TYPED_LIBRARY_CURRENT_FI_COHORT_RECORD_ENV}" MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR "Typed FI INPUT record environment name is invalid")
endif()
if(NOT TARGET ${source_typed_native_fi_runner})
    message(FATAL_ERROR "Typed FI registration requires the dedicated same-provider receiver target")
endif()
if(NOT Python3_EXECUTABLE)
    message(FATAL_ERROR "Typed FI registration requires the caller's actual Python3 executable")
endif()
set(source_typed_fi_args --typed-library
    --binary "$<TARGET_FILE:${source_typed_native_fi_runner}>"
    --input-root "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}"
    --typed-cohort-input "${SOURCE_TYPED_LIBRARY_CURRENT_FI_COHORT_INPUT}"
    --typed-cohort-record-env "${SOURCE_TYPED_LIBRARY_CURRENT_FI_COHORT_RECORD_ENV}"
    --typed-guard-module "${SOURCE_TYPED_LIBRARY_CURRENT_FI_GUARD_MODULE}"
    --typed-guard-record "${SOURCE_TYPED_LIBRARY_CURRENT_FI_GUARD_RECORD}"
    --evidence "${SOURCE_TYPED_LIBRARY_CURRENT_FI_EVIDENCE_ROOT}")
foreach(source_typed_fi_mode IN ITEMS 1 2 3)
    foreach(source_typed_fi_kind IN ITEMS compiler runtime)
        set(source_typed_fi_test "${source_typed_native_fi_runner}_${source_typed_fi_kind}_mode${source_typed_fi_mode}")
        add_test(NAME ${source_typed_fi_test} COMMAND "${Python3_EXECUTABLE}"
            "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/scripts/source_product_consumer_allocation_faults.py"
            ${source_typed_fi_args} --mode ${source_typed_fi_mode} --kind ${source_typed_fi_kind} --jobs 8)
        set_tests_properties(${source_typed_fi_test} PROPERTIES
            WORKING_DIRECTORY "${source_typed_native_material}" TIMEOUT 600 PROCESSORS 8
            LABELS "unit;xir;typed-library;packet;native;mixed;allocation-fault;receiver-only")
    endforeach()
    foreach(source_typed_fi_axis IN ITEMS allocated live work)
        foreach(source_typed_fi_cut IN ITEMS exact minus1)
            set(source_typed_fi_axis_test "${source_typed_native_fi_runner}_axis_${source_typed_fi_axis}_${source_typed_fi_cut}_mode${source_typed_fi_mode}")
            add_test(NAME ${source_typed_fi_axis_test} COMMAND "${Python3_EXECUTABLE}"
                "${SOURCE_TYPED_LIBRARY_CURRENT_FORMAL_ROOT}/scripts/source_product_consumer_allocation_axes.py"
                ${source_typed_fi_args} --mode ${source_typed_fi_mode}
                --axis ${source_typed_fi_axis} --cut ${source_typed_fi_cut})
            set_tests_properties(${source_typed_fi_axis_test} PROPERTIES
                WORKING_DIRECTORY "${source_typed_native_material}" TIMEOUT 120 PROCESSORS 1 RUN_SERIAL TRUE
                LABELS "unit;xir;typed-library;packet;native;mixed;compiler-resource-boundary;receiver-only")
        endforeach()
    endforeach()
endforeach()
# No writer/Source-product invocation is in either test command. The dedicated
# executable consumes the exact four once-generated materials through its target.
# This registration is prepared, not adopted/configured/built/executed/qualified.
