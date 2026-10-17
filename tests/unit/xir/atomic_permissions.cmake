if(NOT TARGET test_xir_mixed_atomic_permissions)
    message(FATAL_ERROR "The independent Atomic Source permission consumer must exist before registering cases")
endif()
foreach(case IN ITEMS
    const-i64
    const-alias-i64
    root-param-i64
    root-local-alias-i64
    worker-local-i64
    mixed-library-pure
    var-direct-i64
    var-transitive-i64
    var-in-worker-arg-i64
    var-rebind-i64
    var-local-module-alias-i64
    nested-worker-i64
    const-f64
    root-param-f64
    var-direct-f64
    const-bool
    root-param-bool
    var-direct-bool
)
    set(name test_xir_mixed_atomic_permissions_${case})
    add_test(NAME ${name} COMMAND test_xir_mixed_atomic_permissions ${case})
    set_tests_properties(${name} PROPERTIES TIMEOUT 120 PROCESSORS 1 COST 2
        LABELS "unit;xir;mixed;permissions;atomic;source;ownership")
endforeach()
