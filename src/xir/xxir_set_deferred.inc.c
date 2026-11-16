/* Set retirement is published only after the actual receiving binding moves. */
XR_FUNC XrXirValueStatus xr_xir_set_add_deferred(const XrXirValuePlace *place,
    const XrXirValue *element,XrXirValueAdmission *admission,XrXirValue *retired) {
    if(!element || !hash_admission_current(admission) || !hash_retired_valid(place,retired) ||
        retired==element)return XR_XIR_VALUE_BAD_ARGUMENT;
    /* NULL value selects the true SET receiver in hash_place_current. */
    xr_xir_value_graph_begin();
    XrXirValueStatus status=hash_upsert(place,element,NULL,admission,retired);
    xr_xir_value_graph_end();return status;
}
