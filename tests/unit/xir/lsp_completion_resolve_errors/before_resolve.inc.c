XR_FUNC XrJsonValue *xlsp_handle_td_completion_resolve(XrLspServer *server, XrJsonValue *params) {
    (void)server;
    return xjson_clone(params);
}
