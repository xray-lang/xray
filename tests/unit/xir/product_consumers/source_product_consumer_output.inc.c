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
#include "fixtures/canonical_initializer/expected_output.h"

typedef struct ConsumerOutput {
    size_t groups, bytes, reject_at;
    XrXirOutputStatus reject_status;
    bool rejected;
    char text[256];
} ConsumerOutput;

static bool consumer_text_case(void) {
    return !strcmp(XR_CONSUMER_NAME, "text_program") || consumer_initializer_case();
}

static size_t consumer_output_golden_length(void) {
    return consumer_initializer_case() ? sizeof(consumer_initializer_golden) - 1 : sizeof(consumer_text_golden) - 1;
}

static const char *consumer_output_golden(void) {
    return consumer_initializer_case() ? consumer_initializer_golden : consumer_text_golden;
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
    CHECK(output->bytes <= consumer_output_golden_length());
    CHECK(length <= consumer_output_golden_length() - output->bytes);
    CHECK(!memcmp(bytes, consumer_output_golden() + output->bytes, length));
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
    CHECK(output->groups < (consumer_initializer_case() ? 1u : 3u) && !output->rejected);
    if (consumer_initializer_case()) {
        CHECK(group->count == 1 && group->values[0].type == XR_XIR_I64 && group->values[0].payload == 42);
    } else if (!output->groups) {
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
    CHECK(!output->rejected && output->groups == (consumer_initializer_case() ? 1u : 3u));
    CHECK(output->bytes == consumer_output_golden_length());
    CHECK(!memcmp(output->text, consumer_output_golden(), output->bytes));
}
