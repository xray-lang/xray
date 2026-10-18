/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * test_repl.c - Original REPL expectations through the actual Source owner
 */
#include "source_repl_original_support.h"
#include "source_repl_original_hosted.h"

TEST(globals_dict_set_get_round_trip) {
    original_open();ASSERT_TRUE(original_eval("var answer = 0\n"));
    XrSourceValue *value=original_scalar(42);ASSERT_NOT_NULL(value);
    ASSERT_EQ_INT(xray_source_repl_write(fixture.owner,"answer",value),XR_SOURCE_OK);
    ASSERT_NOT_NULL(original_find("answer"));ASSERT_EQ_INT(original_names(),1);
    ASSERT_TRUE(original_integer("answer",42));ASSERT_EQ_INT(original_births(false,NULL),1);ASSERT_NULL(original_find("it"));
}
TEST(globals_dict_overwrite_keeps_count) {
    original_open();ASSERT_TRUE(original_eval("var x = 0\n"));
    XrSourceValue *a=original_scalar(1),*b=original_scalar(2);ASSERT_NOT_NULL(a);ASSERT_NOT_NULL(b);
    ASSERT_EQ_INT(xray_source_repl_write(fixture.owner,"x",a),XR_SOURCE_OK);
    ASSERT_EQ_INT(xray_source_repl_write(fixture.owner,"x",b),XR_SOURCE_OK);
    ASSERT_EQ_INT(original_names(),1);ASSERT_TRUE(original_integer("x",2));ASSERT_EQ_INT(original_births(false,NULL),1);
}
TEST(globals_dict_missing_key_returns_null) {
    original_open();XrSourceValue *missing=NULL,*value=original_scalar(42);ASSERT_NOT_NULL(value);
    ASSERT_NULL(original_find("ghost"));ASSERT_TRUE(xray_source_repl_read(fixture.owner,"ghost",&missing)!=XR_SOURCE_OK);
    ASSERT_NULL(missing);ASSERT_TRUE(xray_source_repl_write(fixture.owner,"ghost",value)!=XR_SOURCE_OK);
    ASSERT_EQ_INT(original_names(),0);ASSERT_EQ_INT(original_births(false,NULL),0);
}
TEST(repl_cross_input_call_rejects_missing_type_authority) {
    original_open();ASSERT_TRUE(original_eval("fn inc(n: i64) -> i64 { return n + 1 }\n"));
    XrXirSourceGenerationView view={0};
    ASSERT_EQ_INT(xr_xir_compile_source_generation_view(fixture.owner->submission->source,1,&view),XR_XIR_OK);
    XrXirModule *checked=(XrXirModule *)xr_xir_compile_artifact_module(view.result->checked);
    ASSERT_NOT_NULL(checked);const XrXirTypes *types=checked->types;ASSERT_NOT_NULL(types);
    checked->types=NULL;XrSourceStatus rejected=original_submit("var r = inc(10)\n",false);checked->types=types;
    ASSERT_TRUE(rejected!=XR_SOURCE_OK);ASSERT_NULL(original_find("r"));ASSERT_EQ_INT(original_births(false,NULL),1);
    ASSERT_TRUE(original_eval("var r = inc(10)\n"));ASSERT_TRUE(original_integer("r",11));
}
TEST(repl_cross_input_call_rejects_invalid_symbol_authority) {
    original_open();ASSERT_TRUE(original_eval("fn inc(n: i64) -> i64 { return n + 1 }\n"));
    XrXirSourceGenerationView view={0};
    ASSERT_EQ_INT(xr_xir_compile_source_generation_view(fixture.owner->submission->source,1,&view),XR_XIR_OK);
    XrXirModule *checked=(XrXirModule *)xr_xir_compile_artifact_module(view.result->checked);
    ASSERT_NOT_NULL(checked);uint32_t count=checked->function_count;ASSERT_TRUE(count>0);
    checked->function_count=0;XrSourceStatus rejected=original_submit("var r = inc(10)\n",false);checked->function_count=count;
    ASSERT_TRUE(rejected!=XR_SOURCE_OK);ASSERT_NULL(original_find("r"));ASSERT_EQ_INT(original_births(false,NULL),1);
    ASSERT_TRUE(original_eval("var r = inc(10)\n"));ASSERT_TRUE(original_integer("r",11));
}
TEST(repl_symbols_new_and_free) {
    original_open();ASSERT_NOT_NULL(fixture.owner);ASSERT_EQ_INT(original_names(),0);
}
TEST(repl_symbols_free_null_is_noop) {
    XrSourceRepl *owner=NULL;XrSourceEvent cleanup={0};
    ASSERT_EQ_INT(xray_source_repl_drop(&owner,&cleanup),XR_SOURCE_OK);ASSERT_NULL(owner);ASSERT_EQ_INT(cleanup.state,XR_SOURCE_RUN_READY);
}
TEST(repl_symbols_of_null_isolate) {
    const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    ASSERT_EQ_INT(xray_source_repl_bindings(NULL,&rows,&count),XR_SOURCE_BAD_ARGUMENT);ASSERT_NULL(rows);ASSERT_EQ_INT(count,0);
}
TEST(repl_symbol_cname_null_safety) {
    original_open();XrSourceValue *value=NULL;
    ASSERT_EQ_INT(xray_source_repl_read(fixture.owner,NULL,&value),XR_SOURCE_BAD_ARGUMENT);ASSERT_NULL(value);
}
TEST(repl_eval_let_registers_symbol) {
    original_open();ASSERT_TRUE(original_eval("var x = 42\n"));const XrSourceReplBindingInfo *row=original_find("x");
    ASSERT_NOT_NULL(row);ASSERT_FALSE(row->is_const);ASSERT_TRUE(original_integer("x",42));
}
TEST(repl_eval_const_marks_is_const) {
    original_open();ASSERT_TRUE(original_eval("const PI = 3.14\n"));const XrSourceReplBindingInfo *row=original_find("PI");
    ASSERT_NOT_NULL(row);ASSERT_TRUE(row->is_const);
}
TEST(repl_eval_function_registers_symbol) {
    original_open();ASSERT_TRUE(original_eval("fn double(n: i64) -> i64 { return n * 2 }\n"));
    const XrSourceReplBindingInfo *row=original_find("double");ASSERT_NOT_NULL(row);ASSERT_FALSE(row->is_const);ASSERT_EQ_INT(row->kind,XR_SOURCE_REPL_FUNCTION);
}
TEST(repl_eval_let_and_const_round_trip) {
    original_open();ASSERT_TRUE(original_eval("var x = 1\nconst Y = 2\n"));ASSERT_EQ_INT(original_names(),2);
    const XrSourceReplBindingInfo *x=original_find("x"),*y=original_find("Y");ASSERT_NOT_NULL(x);ASSERT_NOT_NULL(y);ASSERT_FALSE(x->is_const);ASSERT_TRUE(y->is_const);
}
TEST(repl_cross_input_symbol_resolves) {
    original_open();ASSERT_TRUE(original_eval("var x = 42\n"));ASSERT_TRUE(original_eval("var y = x + 1\n"));
    ASSERT_NOT_NULL(original_find("x"));ASSERT_NOT_NULL(original_find("y"));ASSERT_TRUE(original_integer("y",43));
}
TEST(repl_cross_input_function_call) {
    original_open();ASSERT_TRUE(original_eval("fn inc(n: i64) -> i64 { return n + 1 }\n"));ASSERT_TRUE(original_eval("var r = inc(10)\n"));
    ASSERT_NOT_NULL(original_find("inc"));ASSERT_NOT_NULL(original_find("r"));ASSERT_TRUE(original_integer("r",11));
}
TEST(repl_cross_input_function_reads_shared) {
    original_open();ASSERT_TRUE(original_eval("var x = 10\n"));ASSERT_TRUE(original_eval("fn getx() -> i64 { return x }\n"));
    ASSERT_TRUE(original_eval("var r = getx()\n"));ASSERT_TRUE(original_integer("r",10));
}
TEST(repl_cross_input_function_mutates_shared) {
    original_open();ASSERT_TRUE(original_eval("var counter = 0\n"));ASSERT_TRUE(original_eval("fn bump() -> i64 { counter = counter + 1; return counter }\n"));
    ASSERT_TRUE(original_eval("var r1 = bump()\n"));ASSERT_TRUE(original_eval("var r2 = bump()\n"));
    ASSERT_TRUE(original_integer("r1",1));ASSERT_TRUE(original_integer("r2",2));ASSERT_TRUE(original_integer("counter",2));
}
TEST(repl_redefinition_replaces_visible_binding) {
    original_open();ASSERT_TRUE(original_eval("var x = 1\n"));ASSERT_TRUE(original_eval("var x = 2\n"));
    ASSERT_EQ_INT(original_names(),1);ASSERT_TRUE(original_integer("x",2));
}
TEST(repl_function_calls_function_cross_input) {
    original_open();ASSERT_TRUE(original_eval("fn b() -> i64 { return 100 }\n"));ASSERT_TRUE(original_eval("fn a() -> i64 { return b() + 1 }\n"));
    ASSERT_TRUE(original_eval("var r = a()\n"));ASSERT_TRUE(original_integer("r",101));
}
TEST(repl_function_recursive_self_reference) {
    original_open();ASSERT_TRUE(original_eval("fn fact(n: i64) -> i64 {\n  if (n <= 1) { return 1 }\n  return n * fact(n - 1)\n}\n"));
    ASSERT_TRUE(original_eval("var r = fact(5)\n"));ASSERT_TRUE(original_integer("r",120));
}
TEST(repl_function_mutates_array_cross_input) {
    original_open();ASSERT_TRUE(original_eval("var arr: Array<i64> = []\n"));ASSERT_TRUE(original_eval("fn push_one() { arr.push(1) }\n"));
    ASSERT_TRUE(original_eval("push_one(); push_one()\n"));ASSERT_TRUE(original_eval("var n = len(arr)\n"));ASSERT_TRUE(original_integer("n",2));
}
TEST(repl_class_instantiation_cross_input) {
    original_open();ASSERT_NE(original_submit("class Point {var x:i64;var y:i64;init(x:i64,y:i64){this.x=x;this.y=y}}\n",false),XR_SOURCE_OK);
    ASSERT_EQ_INT(original_names(),0);ASSERT_TRUE(original_eval("class Point {\n  x: i64; y: i64\n  init(x: i64, y: i64) { this.x = x; this.y = y }\n}\n"));
    ASSERT_TRUE(original_eval("var px = Point(7, 8).x\n"));ASSERT_TRUE(original_integer("px",7));
}
TEST(repl_auto_echo_compiles_bare_expression) {
    original_open();ASSERT_TRUE(original_eval("1 + 1\n"));ASSERT_STR_EQ(fixture.output,"2\n");
}
TEST(repl_auto_echo_creates_it_binding) {
    original_open();ASSERT_NE(original_submit("null\n",false),XR_SOURCE_OK);ASSERT_NULL(original_find("it"));ASSERT_TRUE(original_eval("(fn() -> i64? { return null })()\n"));ASSERT_EQ_INT(original_names(),0);ASSERT_EQ_INT(original_births(true,NULL),1);
    const XrSourceReplBindingInfo *it=original_find("it");ASSERT_NOT_NULL(it);ASSERT_TRUE(it->implicit_result);ASSERT_STR_EQ(fixture.output,"");
}
TEST(repl_auto_echo_it_chaining) {
    original_open();ASSERT_TRUE(original_eval("1 + 2\n"));ASSERT_TRUE(original_eval("it * 10\n"));ASSERT_EQ_INT(original_names(),0);
    ASSERT_EQ_INT(original_births(true,NULL),2);ASSERT_TRUE(original_integer("it",30));
}
TEST(repl_it_can_change_static_type) {
    original_open();ASSERT_TRUE(original_eval("1\n"));ASSERT_TRUE(original_eval("\"hello\"\n"));ASSERT_TRUE(original_eval("var n = len(it)\n"));
    ASSERT_TRUE(original_integer("n",5));
}
TEST(repl_unit_result_does_not_replace_it) {
    original_open();ASSERT_TRUE(original_eval("var calls = 0\nfn test() { calls = calls + 1 }\n"));ASSERT_TRUE(original_eval("test\n"));
    ASSERT_TRUE(original_eval("test()\n"));ASSERT_TRUE(original_eval("it()\n"));ASSERT_EQ_INT(original_births(true,NULL),1);ASSERT_TRUE(original_integer("calls",2));
}
TEST(repl_it_reference_is_a_typed_snapshot) {
    original_open();ASSERT_TRUE(original_eval("1\n"));ASSERT_TRUE(original_eval("fn previous() -> i64 { return it }\n"));ASSERT_TRUE(original_eval("\"later\"\n"));
    ASSERT_TRUE(original_eval("var saved = previous()\n"));ASSERT_TRUE(original_integer("saved",1));
}
TEST(repl_unit_before_value_does_not_create_it) {
    original_open();ASSERT_TRUE(original_eval("fn noop() {}\n"));ASSERT_TRUE(original_eval("noop()\n"));ASSERT_NULL(original_find("it"));
}
TEST(repl_it_is_reserved_for_implicit_results) {
    original_open();ASSERT_NE(original_submit("var it = 1\n",false),XR_SOURCE_OK);ASSERT_NULL(original_find("it"));ASSERT_EQ_INT(original_names(),0);
}
TEST(repl_eval_requires_explicit_valid_memory_identity) {
    original_open();const XrSourceText missing={NULL,NULL,"1 + 2\n",6},malformed={NULL,"<eval>","1 + 2\n",6};
    ASSERT_EQ_INT(xray_source_repl_begin(fixture.owner,&missing,NULL),XR_SOURCE_BAD_ARGUMENT);
    ASSERT_EQ_INT(xray_source_repl_begin(fixture.owner,&malformed,NULL),XR_SOURCE_BAD_ARGUMENT);ASSERT_EQ_INT(original_names(),0);
}
TEST(repl_auto_echo_evaluates_expression_once) {
    original_open();ASSERT_TRUE(original_eval("var counter = 0\nfn next() -> i64 { counter = counter + 1; return counter }\n"));
    ASSERT_TRUE(original_eval("next()\n"));ASSERT_TRUE(original_integer("counter",1));ASSERT_TRUE(original_integer("it",1));
}
TEST(repl_print_vars_empty_is_safe) {
    original_open();ASSERT_EQ_INT(original_names(),0);const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    ASSERT_EQ_INT(xray_source_repl_bindings(fixture.owner,&rows,&count),XR_SOURCE_OK);ASSERT_NULL(rows);ASSERT_EQ_INT(count,0);
}
TEST(repl_print_vars_after_compile_no_crash) {
    original_open();ASSERT_TRUE(original_eval("var x = 1\nconst Y = 2\n"));const XrSourceReplBindingInfo *rows=NULL;size_t count=0;
    ASSERT_EQ_INT(xray_source_repl_bindings(fixture.owner,&rows,&count),XR_SOURCE_OK);ASSERT_EQ_INT(count,2);
    for(size_t i=0;i<count;++i){XrSourceValue *value=NULL;ASSERT_EQ_INT(xray_source_repl_read(fixture.owner,rows[i].name,&value),XR_SOURCE_OK);
        XrSourceStatus status=xray_source_value_format(value,original_output,&fixture);xray_source_value_free(value);ASSERT_EQ_INT(status,XR_SOURCE_OK);}
    ASSERT_TRUE(original_integer("x",1));ASSERT_TRUE(original_integer("Y",2));
    ASSERT_EQ_INT(fixture.output_length,2);
}
TEST(repl_print_type_null_and_empty_safe) {
    original_open();ASSERT_NE(original_submit(NULL,true),XR_SOURCE_OK);ASSERT_NE(original_submit("",true),XR_SOURCE_OK);ASSERT_NE(original_submit("   \t  ",true),XR_SOURCE_OK);
    ASSERT_EQ_INT(original_names(),0);ASSERT_TRUE(original_eval("42\n"));ASSERT_TRUE(original_integer("it",42));
}
TEST(repl_print_type_simple_expression) {
    original_open();ASSERT_EQ_INT(original_submit("1 + 2",true),XR_SOURCE_OK);ASSERT_EQ_INT(fixture.event.state,XR_SOURCE_RUN_RETURNED);
    ASSERT_NOT_NULL(strstr(fixture.output,"i64"));ASSERT_NULL(original_find("it"));
}
TEST(repl_old_callables_pin_their_source_generation) {
    original_open();ASSERT_TRUE(original_eval("fn value() -> i64 { return 40 }\n"));ASSERT_TRUE(original_eval("fn before() -> i64 { return value() + 2 }\n"));
    ASSERT_TRUE(original_eval("fn value() -> string { return \"later\" }\n"));ASSERT_TRUE(original_eval("var old = before()\n"));ASSERT_TRUE(original_eval("var latest = value()\n"));
    ASSERT_TRUE(original_integer("old",42));ASSERT_TRUE(original_string("latest","later"));
}
TEST(repl_shadowed_variable_keeps_the_old_typed_owner) {
    original_open();ASSERT_TRUE(original_eval("var x = 11\n"));ASSERT_TRUE(original_eval("fn read() -> i64 { return x }\n"));
    ASSERT_TRUE(original_eval("var x = \"later\"\n"));ASSERT_TRUE(original_eval("var old = read()\n"));ASSERT_TRUE(original_integer("old",11));ASSERT_TRUE(original_string("x","later"));
}
TEST(repl_compile_failure_does_not_publish_a_declaration) {
    original_open();ASSERT_TRUE(original_eval("fn value() -> i64 { return 5 }\n"));ASSERT_NE(original_submit("fn value() -> string { return 1 }\n",false),XR_SOURCE_OK);
    ASSERT_TRUE(original_eval("var answer = value()\n"));ASSERT_TRUE(original_integer("answer",5));uint64_t latest=0;ASSERT_EQ_INT(original_births(false,&latest),2);ASSERT_EQ_INT(latest,3);
}
TEST(repl_runtime_failure_does_not_replace_a_binding) {
    original_open();ASSERT_TRUE(original_eval("fn value() -> i64 { return 5 }\n"));size_t before=original_names();
    ASSERT_EQ_INT(original_submit("fn value() -> string { return \"later\" }\nvar zero = 0\nvar crash = 1 / zero\n",false),XR_SOURCE_OK);
    ASSERT_EQ_INT(fixture.event.state,XR_SOURCE_RUN_PANICKED);ASSERT_EQ_INT(original_names(),before);ASSERT_TRUE(original_eval("var answer = value()\n"));
    ASSERT_TRUE(original_integer("answer",5));uint64_t latest=0;ASSERT_EQ_INT(original_births(false,&latest),2);ASSERT_EQ_INT(latest,3);
}
TEST(repl_generic_source_facts_survive_later_inputs) {
    original_open();ASSERT_TRUE(original_eval("fn identity<T>(x: T) -> T { return x }\nvar first = identity(41)\n"));
    ASSERT_TRUE(original_eval("fn later() -> i64 { return identity(42) }\n"));ASSERT_TRUE(original_eval("var answer = later()\n"));
    ASSERT_TRUE(original_integer("first",41));ASSERT_TRUE(original_integer("answer",42));
}
TEST(repl_published_initializers_are_not_executed_again) {
    original_open();ASSERT_TRUE(original_eval("print(\"init\")\nvar x = 41\n"));ASSERT_TRUE(original_eval("var answer = x + 1\n"));
    ASSERT_STR_EQ(fixture.output,"init\n");ASSERT_TRUE(original_integer("answer",42));
}
TEST(repl_runtime_failure_keeps_prior_binding_side_effects) {
    original_open();ASSERT_TRUE(original_eval("var x = 11\n"));ASSERT_EQ_INT(original_submit("x += 1\nvar zero = 0\nvar crash = 1 / zero\n",false),XR_SOURCE_OK);
    ASSERT_EQ_INT(fixture.event.state,XR_SOURCE_RUN_PANICKED);ASSERT_TRUE(original_integer("x",12));ASSERT_EQ_INT(original_names(),1);
}
TEST(repl_failed_callable_input_cannot_change_published_targets) {
    original_open();ASSERT_TRUE(original_eval("fn apply(f: fn(i64) -> i64, x: i64) -> i64 { return f(x) }\nfn original(x: i64) -> i64 { return x + 1 }\n"));
    ASSERT_EQ_INT(original_submit("fn original(x: i64) -> i64 { return x * 2 }\nvar transient = apply(original, 10)\nvar zero = 0\nvar crash = 1 / zero\n",false),XR_SOURCE_OK);
    ASSERT_EQ_INT(fixture.event.state,XR_SOURCE_RUN_PANICKED);ASSERT_TRUE(original_eval("var answer = apply(original, 10)\n"));
    ASSERT_TRUE(original_integer("answer",11));ASSERT_NULL(original_find("transient"));
}

TEST_MAIN_BEGIN()
RUN_TEST(product_profiles_install_exact_authority_and_reject_missing_profile);
RUN_TEST(sync_root_elides_coroutine_with_ordinary_allocation);
RUN_TEST(repl_profile_clears_each_call);
RUN_TEST(globals_dict_set_get_round_trip);
RUN_TEST(globals_dict_overwrite_keeps_count);
RUN_TEST(globals_dict_missing_key_returns_null);
RUN_TEST(repl_cross_input_call_rejects_missing_type_authority);
RUN_TEST(repl_cross_input_call_rejects_invalid_symbol_authority);
RUN_TEST(repl_symbols_new_and_free);
RUN_TEST(repl_symbols_free_null_is_noop);
RUN_TEST(repl_symbols_of_null_isolate);
RUN_TEST(repl_symbol_cname_null_safety);
RUN_TEST(repl_eval_let_registers_symbol);
RUN_TEST(repl_eval_const_marks_is_const);
RUN_TEST(repl_eval_function_registers_symbol);
RUN_TEST(repl_eval_let_and_const_round_trip);
RUN_TEST(repl_cross_input_symbol_resolves);
RUN_TEST(repl_cross_input_function_call);
RUN_TEST(repl_cross_input_function_reads_shared);
RUN_TEST(repl_cross_input_function_mutates_shared);
RUN_TEST(repl_redefinition_replaces_visible_binding);
RUN_TEST(repl_function_calls_function_cross_input);
RUN_TEST(repl_function_recursive_self_reference);
RUN_TEST(repl_function_mutates_array_cross_input);
RUN_TEST(repl_class_instantiation_cross_input);
RUN_TEST(repl_auto_echo_compiles_bare_expression);
RUN_TEST(repl_auto_echo_creates_it_binding);
RUN_TEST(repl_auto_echo_it_chaining);
RUN_TEST(repl_it_can_change_static_type);
RUN_TEST(repl_unit_result_does_not_replace_it);
RUN_TEST(repl_it_reference_is_a_typed_snapshot);
RUN_TEST(repl_unit_before_value_does_not_create_it);
RUN_TEST(repl_it_is_reserved_for_implicit_results);
RUN_TEST(repl_eval_requires_explicit_valid_memory_identity);
RUN_TEST(repl_auto_echo_evaluates_expression_once);
RUN_TEST(repl_print_vars_empty_is_safe);
RUN_TEST(repl_print_vars_after_compile_no_crash);
RUN_TEST(repl_print_type_null_and_empty_safe);
RUN_TEST(repl_print_type_simple_expression);
RUN_TEST(repl_old_callables_pin_their_source_generation);
RUN_TEST(repl_shadowed_variable_keeps_the_old_typed_owner);
RUN_TEST(repl_compile_failure_does_not_publish_a_declaration);
RUN_TEST(repl_runtime_failure_does_not_replace_a_binding);
RUN_TEST(repl_generic_source_facts_survive_later_inputs);
RUN_TEST(repl_published_initializers_are_not_executed_again);
RUN_TEST(repl_runtime_failure_keeps_prior_binding_side_effects);
RUN_TEST(repl_failed_callable_input_cannot_change_published_targets);
TEST_MAIN_END()
