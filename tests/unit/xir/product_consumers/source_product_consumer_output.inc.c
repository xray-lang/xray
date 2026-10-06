/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * source_product_consumer_output.inc.c - Independent typed and byte output
 *
 * KEY CONCEPT:
 *   Each call preserves the original oracle through the public output renderer.
 */
#include "xir/xxir_output.h"
#include "fixtures/text_program/expected_output.h"

typedef struct ConsumerOutput {
    size_t groups, bytes, reject_at;
    XrXirOutputStatus reject_status;
    bool rejected;
    char text[256];
} ConsumerOutput;

static bool consumer_text_case(void) {
    return !strcmp(XR_CONSUMER_NAME, "text_program");
}

static void consumer_output_reset(ConsumerOutput *output) {
    memset(output, 0, sizeof(*output));
    output->reject_at = SIZE_MAX;
}

static void consumer_output_string(const XrXirValue *value, const char *expected) {
    const char *bytes = NULL;
    size_t length = 0;
    CHECK(value->type == XR_XIR_STRING && !value->reserved && xr_xir_string_view(value, &bytes, &length));
    CHECK(length == strlen(expected) && !memcmp(bytes, expected, length));
}

static XrXirOutputStatus consumer_output_bytes(void *context, XrXirOutputStream stream,
    const char *bytes, size_t length) {
    ConsumerOutput *output = context;
    CHECK(stream == XR_XIR_STDOUT && bytes && length);
    CHECK(output->bytes <= sizeof(consumer_text_golden) - 1);
    CHECK(length <= sizeof(consumer_text_golden) - 1 - output->bytes);
    CHECK(!memcmp(bytes, consumer_text_golden + output->bytes, length));
    if (output->groups == output->reject_at) {
        output->rejected = true;
        return output->reject_status;
    }
    CHECK(length <= sizeof(output->text) - output->bytes);
    memcpy(output->text + output->bytes, bytes, length);
    output->bytes += length;
    return XR_XIR_OUTPUT_OK;
}

static XrXirOutputStatus consumer_output_group(void *context, const XrXirOutputGroup *group) {
    XrXirOutputSink *sink = context;
    ConsumerOutput *output = sink->context;
    CHECK(group && group->stream == XR_XIR_STDOUT && group->line && group->values);
    CHECK(output->groups < 3 && !output->rejected);
    if (!output->groups) {
        CHECK(group->count == 1);
        consumer_output_string(&group->values[0], "hello, world");
    } else if (output->groups == 1) {
        CHECK(group->count == 3);
        consumer_output_string(&group->values[0], "answer=42");
        CHECK(group->values[1].type == XR_XIR_BOOL && group->values[1].payload == 1);
        CHECK(group->values[2].type == XR_XIR_RUNE && group->values[2].payload == 'x');
    } else {
        CHECK(group->count == 5);
        consumer_output_string(&group->values[0], "code");
        CHECK(group->values[1].type == XR_XIR_I64 && group->values[1].payload == 3);
        consumer_output_string(&group->values[2], "xxx");
        consumer_output_string(&group->values[3], "yes");
        consumer_output_string(&group->values[4], "no");
    }
    ++output->groups;
    return xr_xir_output_render(sink, group);
}

static XrXirInstanceConfig consumer_config(ConsumerOutput *output, XrXirOutputSink *sink) {
    XrXirInstanceConfig config;
    CHECK(xr_xir_instance_config_init(&config, sizeof(config)) == XR_XIR_CALL_READY);
    config.value_limit = UINT64_C(1048576);
    consumer_output_reset(output);
    if (consumer_text_case()) {
        *sink = (XrXirOutputSink){XR_XIR_CALL_ABI_VERSION, 0, consumer_output_bytes, output, sizeof(output->text)};
        config.output = (XrXirOutputProvider){XR_XIR_CALL_ABI_VERSION, 0, consumer_output_group, sink};
    }
    return config;
}

static void consumer_output_complete(const ConsumerOutput *output) {
    if (!consumer_text_case()) {
        CHECK(!output->groups && !output->bytes);
        return;
    }
    CHECK(!output->rejected && output->groups == 3 && output->bytes == sizeof(consumer_text_golden) - 1);
    CHECK(!memcmp(output->text, consumer_text_golden, output->bytes));
}
