/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xast_types.h - AST node type enums
 *
 * KEY CONCEPT:
 *   Defines all AST node types and operator types for the parser.
 */

#ifndef XAST_TYPES_H
#define XAST_TYPES_H

#include <stdbool.h>

// Operator types for operator overloading
typedef enum {
    OPTYPE_ADD,           // +
    OPTYPE_SUB,           // -
    OPTYPE_MUL,           // *
    OPTYPE_DIV,           // /
    OPTYPE_MOD,           // %
    OPTYPE_BAND,          // & (bitwise and)
    OPTYPE_BOR,           // | (bitwise or)
    OPTYPE_BXOR,          // ^ (bitwise xor)
    OPTYPE_EQ,            // ==
    OPTYPE_NE,            // !=
    OPTYPE_LT,            // <
    OPTYPE_LE,            // <=
    OPTYPE_GT,            // >
    OPTYPE_GE,            // >=
    OPTYPE_UNARY,         // Unary: -, !, ~
    OPTYPE_SUBSCRIPT,     // Subscript: []
    OPTYPE_SUBSCRIPT_SET  // Subscript assign: []=
} OperatorType;

// AST node types
typedef enum {
    // Literal nodes
    AST_LITERAL_INT,     // Integer: 123
    AST_LITERAL_FLOAT,   // Float: 3.14
    AST_LITERAL_BIGINT,  // BigInt: 123n
    AST_LITERAL_STRING,  // String: "hello"
    AST_LITERAL_REGEX,   // Regex: /pattern/flags
    AST_LITERAL_NULL,    // null
    AST_LITERAL_TRUE,    // true
    AST_LITERAL_FALSE,   // false

    // Template string
    AST_TEMPLATE_STRING,  // `Hello, ${name}!`

    // Binary - arithmetic
    AST_BINARY_ADD,  // a + b
    AST_BINARY_SUB,  // a - b
    AST_BINARY_MUL,  // a * b
    AST_BINARY_DIV,  // a / b
    AST_BINARY_MOD,  // a % b

    // Binary - bitwise
    AST_BINARY_BAND,    // a & b
    AST_BINARY_BOR,     // a | b
    AST_BINARY_BXOR,    // a ^ b
    AST_BINARY_LSHIFT,  // a << b
    AST_BINARY_RSHIFT,  // a >> b

    // Binary - comparison
    AST_BINARY_EQ,         // a == b
    AST_BINARY_NE,         // a != b
    AST_BINARY_EQ_STRICT,  // a === b
    AST_BINARY_NE_STRICT,  // a !== b
    AST_BINARY_LT,         // a < b
    AST_BINARY_LE,         // a <= b
    AST_BINARY_GT,         // a > b
    AST_BINARY_GE,         // a >= b

    // Binary - logical
    AST_BINARY_AND,  // a && b
    AST_BINARY_OR,   // a || b

    // Ternary and nullish
    AST_TERNARY,           // cond ? true_expr : false_expr
    AST_NULLISH_COALESCE,  // a ?? b
    AST_OPTIONAL_CHAIN,    // obj?.prop
    AST_FORCE_UNWRAP,      // expr! (force unwrap nullable)
    AST_TRY_OPTIONAL,      // try? expr (fold thrown exception to null; type T -> T?)
    AST_TRY_FORCE,         // try! expr (panic on thrown exception; type unchanged)
    AST_AS_EXPR,           // expr as Type (explicit type cast)
    AST_RANGE,             // 1..10

    // Type check
    AST_IS_EXPR,  // x is Type (runtime type check)

    // Unary
    AST_UNARY_NEG,   // -a
    AST_UNARY_NOT,   // !a
    AST_UNARY_BNOT,  // ~a

    // Grouping
    AST_GROUPING,  // (expr)

    // Statement nodes
    AST_EXPR_STMT,   // Expression statement
    AST_PRINT_STMT,  // print statement (builtin)
    AST_BLOCK,       // { ... }

    // Variable nodes
    AST_VAR_DECL,             // let x = 10
    AST_CONST_DECL,           // const PI = 3.14
    AST_VARIABLE,             // x
    AST_ASSIGNMENT,           // x = 10
    AST_COMPOUND_ASSIGNMENT,  // x += 10, x -= 5, etc.
    AST_INC,                  // ++x or x++
    AST_DEC,                  // --x or x--

    // Control flow
    AST_IF_STMT,        // if (cond) {...} else {...}
    AST_WHILE_STMT,     // while (cond) {...}
    AST_FOR_STMT,       // for (init; cond; update) {...}
    AST_FOR_IN_STMT,    // for (item in collection) {...}
    AST_BREAK_STMT,     // break
    AST_CONTINUE_STMT,  // continue

    // Function nodes
    AST_FUNCTION_DECL,  // fn add(a, b) {...}
    AST_FUNCTION_EXPR,  // (a, b) => a + b
    AST_CALL_EXPR,      // add(1, 2)
    AST_RETURN_STMT,    // return expr

    // Array nodes
    AST_ARRAY_LITERAL,  // [1, 2, 3]
    AST_INDEX_GET,      // arr[0]
    AST_INDEX_SET,      // arr[0] = 10
    AST_SLICE_EXPR,     // arr[start:end]
    AST_MEMBER_ACCESS,  // arr.length, arr.push

    // Data structure literals
    AST_OBJECT_LITERAL,  // {a: 1, b: 2} - static structure
    AST_MAP_LITERAL,     // {"a"=> 1, "b"=> 2} - dynamic container
    AST_SET_LITERAL,     // #[1, 2, 3] - set
    AST_TUPLE_LITERAL,   // (a, b, c) - fixed-arity heterogeneous tuple (incl. () unit, (a,) unary)

    // OOP nodes
    AST_CLASS_DECL,          // class Dog extends Animal {...}
    AST_STRUCT_DECL,         // struct Point { x: float, y: float }
    AST_STRUCT_LITERAL,      // Point{x: 1.0, y: 2.0}
    AST_INTERFACE_DECL,      // interface Drawable {...}
    AST_INTERFACE_METHOD,    // Interface method signature: draw(): void;
    AST_INTERFACE_PROPERTY,  // Interface property signature: length: int
    AST_FIELD_DECL,          // name: string
    AST_METHOD_DECL,         // greet() {...}
    AST_NEW_EXPR,            // new Dog("Rex")
    AST_THIS_EXPR,           // this
    AST_SUPER_CALL,          // super.greet() or super(args)
    AST_MEMBER_SET,          // obj.field = value

    // Enum nodes
    AST_ENUM_DECL,     // enum Status : int { Success = 200 }
    AST_ENUM_MEMBER,   // Success = 200
    AST_ENUM_ACCESS,   // Status.Success
    AST_ENUM_CONVERT,  // Status(200)
    AST_ENUM_INDEX,    // enum_type.members[idx] (compiler-generated for for-in)

    // Exception handling
    AST_TRY_CATCH,   // try-catch-finally
    AST_THROW_STMT,  // throw expr

    // Module system
    AST_IMPORT_STMT,  // import
    AST_EXPORT_STMT,  // export

    // Destructuring
    AST_DESTRUCTURE_DECL,    // let [a, b] = arr
    AST_DESTRUCTURE_ASSIGN,  // [a, b] = [b, a]

    // Match expression
    AST_MATCH_EXPR,  // match (x) { 1 => "one", _ => "other" }
    AST_MATCH_ARM,   // 1 => "one"

    // Pattern matching
    AST_PATTERN_LITERAL,   // 1, "hello", true
    AST_PATTERN_RANGE,     // 1..10
    AST_PATTERN_WILDCARD,  // _
    AST_PATTERN_MULTI,     // 1 | 2 | 3 (alternation list)
    AST_PATTERN_TUPLE,     // (a, b) / (0, _) / ((x, y), z) — positional tuple destructure
    AST_PATTERN_ADT,       // Shape.Circle(r) — ADT variant destructure

    // Spread element: `...expr` inside tuple literal or call argument list.
    // Statically expanded — the source must be a tuple of known arity.
    AST_SPREAD_EXPR,

    // Type alias
    AST_TYPE_ALIAS,  // type User = { name: string, age: int }

    // Coroutine nodes
    AST_GO_EXPR,         // go fn() or go { block }
    AST_AWAIT_EXPR,      // await / await all / await any (flags distinguish)
    AST_CHANNEL_NEW,     // Channel() or Channel(10)
    AST_CHAN_SEND,       // ch.send(value)
    AST_CHAN_RECV,       // ch.recv()
    AST_SELECT_STMT,     // select { case ... }
    AST_SELECT_CASE,     // msg from ch => ...
    AST_DEFER_STMT,      // defer fn()
    AST_SCOPE_BLOCK,     // scope { ... }
    AST_YIELD_STMT,      // yield - give up execution
    AST_CANCELLED_EXPR,  // cancelled() check
    AST_MOVE_EXPR,       // move var - explicit ownership transfer
    AST_CATCH_EXPR,      // catch! { body } — wraps body in Result.Ok/Err

    // Program node
    AST_PROGRAM  // Root node
} AstNodeType;

// Literal types
typedef enum {
    LITERAL_KIND_INT,
    LITERAL_KIND_FLOAT,
    LITERAL_KIND_BIGINT,
    LITERAL_KIND_STRING,
    LITERAL_KIND_REGEX,
    LITERAL_KIND_BOOL,
    LITERAL_KIND_NULL
} LiteralKind;

// Attribute types
typedef enum {
    ATTR_NONE = 0,
    ATTR_TEST,          // @test
    ATTR_TEST_SKIP,     // @test(skip)
    ATTR_TEST_TIMEOUT,  // @test(timeout: N)
    ATTR_BEFORE_EACH,   // @before_each
    ATTR_AFTER_EACH,    // @after_each
    ATTR_BEFORE_ALL,    // @before_all
    ATTR_AFTER_ALL,     // @after_all
    ATTR_NATIVE,        // @native — implementation provided by C runtime
    ATTR_DEPRECATED,    // @deprecated or @deprecated("message")
} AttributeKind;

// Destructuring pattern types (flat only, no nesting)
typedef enum {
    PATTERN_ARRAY,       // [a, b, c]
    PATTERN_OBJECT,      // {name, age}
    PATTERN_TUPLE,       // (a, b, c) — fixed-arity positional, lowered to .0/.1/...
    PATTERN_IDENTIFIER,  // x
    PATTERN_SKIP         // _
} PatternType;

// Module import types
typedef enum {
    IMPORT_STDLIB,   // import time
    IMPORT_PACKAGE,  // import alice/utils
    IMPORT_FILE,     // import "./helper.xr"
    IMPORT_DIR,      // import "models/user"
} ImportType;

#endif  // XAST_TYPES_H
