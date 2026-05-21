/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xmcp_prompts.c - MCP prompt implementations
 *
 * KEY CONCEPT:
 *   Five predefined prompts for AI-assisted Xray development:
 *   - code-review: review Xray code for best practices
 *   - explain-error: explain compile/runtime errors
 *   - convert-to-xray: convert other language code to Xray
 *   - concurrency-pattern: recommend concurrency patterns
 *   - write-test: generate tests for Xray code
 */

#include "xmcp_prompts.h"
#include "xmcp_server.h"
#include "xmcp_knowledge.h"
#include "../../base/xjson.h"
#include "../../base/xmalloc.h"
#include "../../base/xchecks.h"
#include <string.h>
#include <stdio.h>

/* --------------------------------------------------------------------------
 * Prompt definitions
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    const char *description;
    /* Arguments: up to 4 per prompt */
    int arg_count;
    struct {
        const char *name;
        const char *description;
        bool required;
    } args[4];
} PromptDef;

static const PromptDef PROMPTS[] = {
    {"code-review",
     "Review Xray code for best practices, correctness, and style. "
     "Provides actionable feedback with code examples.",
     1,
     {{"code", "Xray source code to review", true}}},
    {"explain-error",
     "Explain an Xray compile error or runtime error in plain language. "
     "Suggests fixes with corrected code examples.",
     1,
     {{"error", "The error message to explain", true}}},
    {"convert-to-xray",
     "Convert code from another programming language to idiomatic Xray. "
     "Preserves logic while using Xray-specific features.",
     2,
     {{"code", "Source code to convert", true},
      {"sourceLanguage", "Original language (e.g., Python, JavaScript, Go)", false}}},
    {"concurrency-pattern",
     "Recommend the best Xray concurrency pattern for a given task. "
     "Covers channels, go/await, scope, select, and shared const.",
     1,
     {{"description", "Description of what the concurrent code should do", true}}},
    {"write-test",
     "Generate @test functions for the given Xray code. "
     "Uses assert_eq, assert_true, assert_throws, etc.",
     1,
     {{"code", "Xray source code to write tests for", true}}},
};

#define PROMPT_COUNT ((int) (sizeof(PROMPTS) / sizeof(PROMPTS[0])))

XR_FUNC size_t xmcp_prompts_count(void) {
    return (size_t) PROMPT_COUNT;
}

/* --------------------------------------------------------------------------
 * System message content (embedded language knowledge for prompts)
 * -------------------------------------------------------------------------- */

static const char SYSTEM_PREAMBLE[] =
    "You are an expert Xray language assistant. "
    "Xray is a statically-typed scripting language with native concurrency. "
    "Key features:\n"
    "- Types: int, float, string, bool, Array<T>, Map<K,V>, Set<T>, "
    "Channel<T>, Json\n"
    "- Concurrency: go/await, Channel (must be const), scope, select, "
    "shared const\n"
    "- Safety: if it compiles, it's concurrency-safe\n"
    "- OOP: class (extends), struct (value type), interface (implements), "
    "enum\n"
    "- Generics: class Box<T>, fn map<T,U>(...)\n"
    "- Testing: @test annotation, assert_eq/assert_true/assert_throws\n"
    "- Modules: import http/json/time/math/io/os/net/ws/crypto etc.\n\n";

static const char CODE_REVIEW_SYSTEM[] =
    "Review the following Xray code. Check for:\n"
    "1. Correctness: type errors, missing imports, logic bugs\n"
    "2. Concurrency safety: shared mutable state, channel usage\n"
    "3. Style: naming conventions, unnecessary complexity\n"
    "4. Performance: inefficient patterns, unnecessary copies\n"
    "Provide specific line-by-line feedback with corrected code.\n";

static const char EXPLAIN_ERROR_SYSTEM[] =
    "Explain the following Xray error message in plain language.\n"
    "1. What the error means\n"
    "2. Common causes\n"
    "3. How to fix it with a corrected code example\n"
    "Important Xray rules:\n"
    "- Channel must be declared with const, not let\n"
    "- go closures cannot capture let/const variables (pass as params)\n"
    "- Arrow function params MUST have type annotations\n"
    "- No quotes inside ${} interpolation\n";

static const char CONVERT_SYSTEM[] =
    "Convert the following code to idiomatic Xray. Guidelines:\n"
    "1. Use Xray types: int (not number), string (not str)\n"
    "2. Use match instead of switch\n"
    "3. Use for-in range (for i in 0..n) when possible\n"
    "4. Use Channel + go for concurrency instead of threads/async-await\n"
    "5. Use struct for value types, class for reference types\n"
    "6. All function params must have type annotations\n"
    "7. Use const for immutable bindings\n";

static const char CONCURRENCY_SYSTEM[] =
    "Recommend the best Xray concurrency pattern. Options:\n"
    "1. go + await: simple fire-and-wait\n"
    "2. Channel: producer-consumer, pipeline\n"
    "3. scope: structured concurrency (wait for all)\n"
    "4. select: multiplex channels with timeout\n"
    "5. shared const: read-only shared config\n"
    "6. await all / await any: fan-out patterns\n"
    "Rules: Channel must be const. go closures deep-copy params. "
    "Cannot capture let variables in go closures.\n";

static const char WRITE_TEST_SYSTEM[] =
    "Generate @test functions for the given Xray code.\n"
    "1. Use @test annotation before each test function\n"
    "2. Test function signature: fn test_xxx(): void\n"
    "3. Use assert_eq(actual, expected) for equality\n"
    "4. Use assert_true(cond) / assert_false(cond) for booleans\n"
    "5. Use assert_throws((): void => { ... }) for error cases\n"
    "6. Cover normal cases, edge cases, and error cases\n"
    "7. Test names should be descriptive\n";

/* --------------------------------------------------------------------------
 * prompts/list handler
 * -------------------------------------------------------------------------- */

XrJsonValue *xmcp_handle_prompts_list(void) {
    XrJsonValue *result = xjson_new_object();
    XrJsonValue *prompts = xjson_new_array();

    for (int i = 0; i < PROMPT_COUNT; i++) {
        const PromptDef *p = &PROMPTS[i];
        XrJsonValue *prompt = xjson_new_object();
        XJSON_SET_STRING(prompt, "name", p->name);
        XJSON_SET_STRING(prompt, "description", p->description);

        if (p->arg_count > 0) {
            XrJsonValue *args = xjson_new_array();
            for (int j = 0; j < p->arg_count; j++) {
                XrJsonValue *arg = xjson_new_object();
                XJSON_SET_STRING(arg, "name", p->args[j].name);
                XJSON_SET_STRING(arg, "description", p->args[j].description);
                XJSON_SET_BOOL(arg, "required", p->args[j].required);
                xjson_array_push(args, arg);
            }
            xjson_object_set(prompt, "arguments", args);
        }

        xjson_array_push(prompts, prompt);
    }

    xjson_object_set(result, "prompts", prompts);
    return result;
}

/* --------------------------------------------------------------------------
 * prompts/get handler
 * -------------------------------------------------------------------------- */

/* Build a messages array with system + user messages. */
static XrJsonValue *build_prompt_messages(const char *system_text, const char *user_text) {
    XR_DCHECK(system_text != NULL, "build_prompt_messages: NULL system");
    XR_DCHECK(user_text != NULL, "build_prompt_messages: NULL user");

    XrJsonValue *messages = xjson_new_array();

    /* System message with language knowledge */
    XrJsonValue *sys_msg = xjson_new_object();
    XJSON_SET_STRING(sys_msg, "role", "user");

    /* Build combined system content */
    size_t preamble_len = strlen(SYSTEM_PREAMBLE);
    size_t system_len = strlen(system_text);
    size_t total = preamble_len + system_len + 1;
    char *combined = xr_malloc(total);
    if (combined) {
        memcpy(combined, SYSTEM_PREAMBLE, preamble_len);
        memcpy(combined + preamble_len, system_text, system_len);
        combined[total - 1] = '\0';

        XrJsonValue *sys_content = xjson_new_object();
        XJSON_SET_STRING(sys_content, "type", "text");
        xjson_object_set(sys_content, "text", xjson_new_string(combined));
        XrJsonValue *sys_arr = xjson_new_array();
        xjson_array_push(sys_arr, sys_content);
        xjson_object_set(sys_msg, "content", sys_arr);
        xr_free(combined);
    }
    xjson_array_push(messages, sys_msg);

    /* User message with the actual content */
    XrJsonValue *usr_msg = xjson_new_object();
    XJSON_SET_STRING(usr_msg, "role", "user");
    XrJsonValue *usr_content = xjson_new_object();
    XJSON_SET_STRING(usr_content, "type", "text");
    xjson_object_set(usr_content, "text", xjson_new_string(user_text));
    XrJsonValue *usr_arr = xjson_new_array();
    xjson_array_push(usr_arr, usr_content);
    xjson_object_set(usr_msg, "content", usr_arr);
    xjson_array_push(messages, usr_msg);

    return messages;
}

XrJsonValue *xmcp_handle_prompts_get(XmcpServer *server, XrJsonValue *params) {
    (void) server;
    XR_DCHECK(params != NULL, "xmcp_handle_prompts_get: NULL params");

    const char *name = xjson_get_string(params, "name");
    if (!name) {
        XrJsonValue *r = xjson_new_object();
        XJSON_SET_STRING(r, "description", "Error: prompt 'name' is required");
        xjson_object_set(r, "messages", xjson_new_array());
        return r;
    }

    XrJsonValue *arguments = xjson_get_object(params, "arguments");

    /* Dispatch by prompt name */
    const char *system_text = NULL;
    const char *user_arg = NULL;
    const char *user_arg_key = NULL;
    char user_buf[4096];

    if (strcmp(name, "code-review") == 0) {
        system_text = CODE_REVIEW_SYSTEM;
        user_arg_key = "code";
    } else if (strcmp(name, "explain-error") == 0) {
        system_text = EXPLAIN_ERROR_SYSTEM;
        user_arg_key = "error";
    } else if (strcmp(name, "convert-to-xray") == 0) {
        system_text = CONVERT_SYSTEM;
        user_arg_key = "code";
        /* Append source language if provided */
        if (arguments) {
            const char *lang = xjson_get_string(arguments, "sourceLanguage");
            const char *code = xjson_get_string(arguments, "code");
            if (lang && code) {
                snprintf(user_buf, sizeof(user_buf), "Source language: %s\n\n```\n%s\n```", lang,
                         code);
                user_arg = user_buf;
            }
        }
    } else if (strcmp(name, "concurrency-pattern") == 0) {
        system_text = CONCURRENCY_SYSTEM;
        user_arg_key = "description";
    } else if (strcmp(name, "write-test") == 0) {
        system_text = WRITE_TEST_SYSTEM;
        user_arg_key = "code";
    }

    if (!system_text) {
        XrJsonValue *r = xjson_new_object();
        char msg[256];
        snprintf(msg, sizeof(msg), "Unknown prompt: %s", name);
        XJSON_SET_STRING(r, "description", msg);
        xjson_object_set(r, "messages", xjson_new_array());
        return r;
    }

    /* Get user argument if not already set */
    if (!user_arg && arguments && user_arg_key) {
        user_arg = xjson_get_string(arguments, user_arg_key);
    }
    if (!user_arg)
        user_arg = "(no input provided)";

    /* Build result */
    XrJsonValue *result = xjson_new_object();

    /* Find prompt description */
    for (int i = 0; i < PROMPT_COUNT; i++) {
        if (strcmp(PROMPTS[i].name, name) == 0) {
            XJSON_SET_STRING(result, "description", PROMPTS[i].description);
            break;
        }
    }

    xjson_object_set(result, "messages", build_prompt_messages(system_text, user_arg));
    return result;
}
