

# Independently measured complete compiler frontiers for both real VM graphs.
set(rb_expected_normal "454839;33661;3481828;1;280;123;157;401;122;300;122;122;122;122;122;122;122;330")
set(rb_expected_failure "454839;33661;3481201;1;280;123;157;401;122;300;122;122;122;122;122;122;122;330")
foreach(case owner_oom check_oom write_oom read_oom specialize_oom reverify_oom
    lower_oom bind_oom seal_oom budget samequota_retry)

endforeach()
