# Every command owns two independent Instances and exactly one selected prefix.
set(task_cancel_modes body call await)
set(task_cancel_ok_counts 18 24 25)
set(task_cancel_error_counts 17 21 22)
foreach(index RANGE 0 2)
    list(GET task_cancel_modes ${index} mode)
    foreach(outcome ok error)
        list(GET task_cancel_${outcome}_counts ${index} count)
        math(EXPR last "${count} - 1")
        foreach(api cancel stop)
            foreach(prefix RANGE 0 ${last})
                add_test(NAME test_xir_task_cancel_scan_${mode}_${outcome}_${api}_${prefix}
                    COMMAND test_xir_task_cancel_prefix --prefix ${mode} ${outcome} ${api} ${prefix})
                set_tests_properties(test_xir_task_cancel_scan_${mode}_${outcome}_${api}_${prefix}
                    PROPERTIES LABELS "unit;xir;task;ownership;runtime;cancel-prefix" TIMEOUT 60)
            endforeach()
        endforeach()
    endforeach()
endforeach()
