/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xxir_value_display.inc.c - Failure-atomic owned text from immutable values
 *
 * KEY CONCEPT:
 *   The same borrowed reader measures and writes the representation. Runtime
 *   work and temporary bytes use the receiving Domain; no child is mutated.
 */
typedef struct XirDisplaySink {
    XrXirValueAdmission *admission;
    char *bytes;
    size_t used,capacity;
    XrXirValueStatus status;
} XirDisplaySink;
static int display_write(void *context,const void *bytes,size_t length) {
    XirDisplaySink *sink=context;
    if (sink->status!=XR_XIR_VALUE_OK) return 0;
    if (length>SIZE_MAX-sink->used || length==SIZE_MAX) {sink->status=XR_XIR_VALUE_OVERFLOW;return 0;}
    if (!admission_is_compile(sink->admission) && (length+1>sink->admission->work ||
        !xr_xir_domain_work(sink->admission->domain,length+1))) {sink->status=XR_XIR_VALUE_LIMIT;return 0;}
    if (!admission_is_compile(sink->admission)) sink->admission->work-=length+1;
    if (sink->bytes) {
        if (length>sink->capacity-sink->used) {sink->status=XR_XIR_VALUE_BAD_ARGUMENT;return 0;}
        if (length) memcpy(sink->bytes+sink->used,bytes,length);
    }
    sink->used+=length;return 1;
}
XR_FUNC XrXirValueStatus xr_xir_format_text(XrValueFormatReader reader,XrValueFormatNode node,
    XrXirValueAdmission *admission,XrXirValue *output) {
    if (!reader.read || !admission_domain_current(admission) || !unit_value(output))
        return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XirDisplaySink sink={.admission=admission};XrValueFormatSink destination={&sink,display_write};
    if (!xr_value_format_value(reader,node,destination,0))
        return sink.status==XR_XIR_VALUE_OK?XR_XIR_VALUE_BAD_ARGUMENT:sink.status;
    size_t length=sink.used;
    char *bytes=length?xr_xir_domain_allocate(admission->domain,length,&status):NULL;
    if (status!=XR_XIR_VALUE_OK) return status;
    sink.bytes=bytes;sink.capacity=length;sink.used=0;
    if (!xr_value_format_value(reader,node,destination,0) || sink.used!=length)
        status=sink.status==XR_XIR_VALUE_OK?XR_XIR_VALUE_BAD_ARGUMENT:sink.status;
    if (status==XR_XIR_VALUE_OK) status=xr_xir_string_new(admission->domain,bytes,length,output);
    if (bytes) xr_xir_domain_deallocate(admission->domain,bytes,length);
    return status;
}
static XrXirValueStatus display_text_received(const XrXirValue *value,XrXirValueAdmission *admission,XrXirValue *output) {
    XrXirValueStatus status=xr_xir_value_admit(value,(XrXirType)value->type,admission);
    if (status!=XR_XIR_VALUE_OK) return status;
    XrXirFormatContext context={.values=admission};
    status=xr_xir_format_text(xr_xir_value_format_reader(&context),(XrValueFormatNode){value,0},admission,output);
    return status!=XR_XIR_VALUE_OK && context.status!=XR_XIR_VALUE_OK ? context.status : status;
}
static XrXirValueStatus display_text_graph(const XrXirValue *value,XrXirValueAdmission *admission,XrXirValue *output) {
    if (!value || value->type==XR_XIR_UNIT || !admission_domain_current(admission) || !unit_value(output)) return XR_XIR_VALUE_BAD_ARGUMENT;
    XrXirValueAdmission received={0};
    XrXirValueStatus status=storage_admission_begin(value,admission,&received);
    if(status!=XR_XIR_VALUE_OK)return status;
    status=display_text_received(value,&received,output);
    return storage_admission_end(admission,&received,status);
}
XR_FUNC XrXirValueStatus xr_xir_value_display_text(const XrXirValue *value,XrXirValueAdmission *admission,XrXirValue *output) {
    xr_xir_value_graph_begin();
    XrXirValueStatus status=display_text_graph(value,admission,output);
    xr_xir_value_graph_end();return status;
}
