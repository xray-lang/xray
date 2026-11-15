/* Copyright (c) 2026 Xinglei Xu. MIT License.
 * One private constructor owns temporary operands and complete panic text. */
typedef struct MapLiteralMessage {char *bytes;size_t count,capacity;} MapLiteralMessage;
static int map_literal_message_write(void *opaque,const void *bytes,size_t count) {
    MapLiteralMessage *message=opaque;
    if(count>SIZE_MAX-message->count)return 0;
    if(message->bytes) {
        if(message->count>message->capacity || count>message->capacity-message->count)return 0;
        if(count)memcpy(message->bytes+message->count,bytes,count);
    }
    message->count+=count;return 1;
}
static bool map_literal_message(MapLiteralMessage *message,const XrtMapLiteral *literal,
    XrXirMapLiteralDuplicate duplicate,const char *key,size_t length) {
    XrtMapLiteralOrigin first=literal->items[duplicate.first],second=literal->items[duplicate.second];
    char left[32],right[32];
    int a=snprintf(left,sizeof(left),"%s%u:%u and ",literal->path_length?":":"",first.line,first.column);
    int b=snprintf(right,sizeof(right),"%s%u:%u)",literal->path_length?":":"",second.line,second.column);
    if(a<0 || (size_t)a>=sizeof(left) || b<0 || (size_t)b>=sizeof(right))return false;
    XrValueFormatSink sink={message,map_literal_message_write};
    return xr_value_format_text(sink,"duplicate key ") && xr_literal_quote_bytes(sink,key,length) &&
        xr_value_format_text(sink," in Map literal (entries at ") &&
        xr_value_format_write(sink,literal->path,literal->path_length) &&
        xr_value_format_write(sink,left,(size_t)a) &&
        xr_value_format_write(sink,literal->path,literal->path_length) && xr_value_format_write(sink,right,(size_t)b);
}
static XR_NORET void map_literal_duplicate_panic(XrtValueRoot *root,const XrtMapLiteral *literal,
    const XrtMapLiteralInput *inputs,XrXirMapLiteralDuplicate duplicate,uint32_t source) {
    XR_CHECK(duplicate.first<literal->count && duplicate.second<literal->count &&
        !literal->items[duplicate.first].kind && !literal->items[duplicate.second].kind,
        "duplicate ordinals identify two explicit inputs");
    XrXirValue key={XR_XIR_STRING,0,(int64_t)(intptr_t)inputs[duplicate.second].source.key};
    const char *bytes=NULL;size_t length=0;
    if(!xr_xir_string_view(&key,&bytes,&length))xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT,source);
    MapLiteralMessage message={0};
    if(!map_literal_message(&message,literal,duplicate,bytes,length))xrt_value_failure(XR_XIR_VALUE_LIMIT,source);
    message.capacity=message.count;message.count=0;
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    message.bytes=xr_xir_domain_allocate(root->domain,message.capacity,&status);
    if(!message.bytes)xrt_value_failure(status,source);
    bool complete=map_literal_message(&message,literal,duplicate,bytes,length);
    if(complete)status=xr_xir_string_new(root->domain,message.bytes,message.count,&root->pending);
    else status=XR_XIR_VALUE_LIMIT;
    xr_xir_domain_deallocate(root->domain,message.bytes,message.capacity);
    if(status!=XR_XIR_VALUE_OK)xrt_value_failure(status,source);
    xrt_message_panic_take(&root->pending,434,source);
}
XR_FUNC bool xrt_map_literal_valid(const XrtMapLiteral *literal) {
    if(!literal || (literal->path_length!=0)!=(literal->path!=NULL) ||
        (literal->path_length && (memchr(literal->path,0,literal->path_length) ||
        xr_utf8_core_scan_strict((const uint8_t *)literal->path,literal->path_length).error!=XR_UTF8_OK)) ||
        literal->count>65536 || (literal->count && !literal->items))return false;
    for(size_t n=0;n<literal->count;++n) {
        XrtMapLiteralOrigin item=literal->items[n];
        if(item.kind>1 || !item.line || !item.column)return false;
    }
    return true;
}
XRAY_API XrtMap *xrt_map_string_i64_literal(const XrtMapLiteral *literal,const XrtMapLiteralInput *inputs,uint32_t source) {
    XrtValueRoot *root=xrt_value_root_current(source);
    if(!xrt_map_literal_valid(literal) || (literal->count && !inputs))xrt_value_failure(XR_XIR_VALUE_BAD_ARGUMENT,source);
    bool spread=false;
    for(size_t n=0;n<literal->count;++n)spread|=literal->items[n].kind!=0;
    XR_CHECK(!root->pending.type && !root->pending.payload,"Map construction owns one empty pending result");
    XrXirValueAdmission admission=map_admission(root,source);map_reserve(root,source);
    size_t count=literal->count;
    size_t width=2*sizeof(XrXirValue)+(spread?sizeof(XrXirMapLiteralKind):0);
    if(count>SIZE_MAX/width)xrt_value_failure(XR_XIR_VALUE_LIMIT,source);
    XrXirValueStatus status=XR_XIR_VALUE_OK;
    XrXirValue *input=count?xr_xir_domain_allocate(root->domain,count*width,&status):NULL;
    if(count && !input)xrt_value_failure(status,source);
    XrXirMapLiteralKind *kinds=spread?(XrXirMapLiteralKind *)(input+2*count):NULL;
    for(size_t n=0;n<count;++n) {
        bool expanded=literal->items[n].kind!=0;
        if(kinds)kinds[n]=expanded?XR_XIR_MAP_LITERAL_SPREAD:XR_XIR_MAP_LITERAL_ENTRY;
        input[n]=expanded?(XrXirValue){0}:(XrXirValue){XR_XIR_STRING,0,(int64_t)(intptr_t)inputs[n].source.key};
        input[count+n]=expanded?map_value(inputs[n].source.spread):(XrXirValue){XR_XIR_I64,0,inputs[n].value};
    }
    XrXirMapLiteralDuplicate duplicate={0};
    status=xr_xir_map_literal_new((XrXirType)XR_XIR_CONSTRUCTED_TYPE_BASE,input,count?input+count:NULL,
        kinds,count,&admission,&root->pending,&duplicate);
    if(input)xr_xir_domain_deallocate(root->domain,input,count*width);
    if(status==XR_XIR_VALUE_DUPLICATE_KEY)map_literal_duplicate_panic(root,literal,inputs,duplicate,source);
    if(status!=XR_XIR_VALUE_OK)xrt_value_failure(status,source);
    return xrt_map_publish_pending(source);
}
