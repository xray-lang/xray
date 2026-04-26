/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xray_embedding.h - C embedding API for integrating Xray into C/C++ projects
 */

#ifndef XRAY_EMBEDDING_H
#define XRAY_EMBEDDING_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif  // ========== Type Definitions ==========

typedef struct XrayVM XrayVM;

// C function type
typedef int (*XrayCFunction)(XrayVM *vm);

// Error codes
#define XRAY_OK 0
#define XRAY_ERRRUN 1     // Runtime error
#define XRAY_ERRSYNTAX 2  // Syntax error
#define XRAY_ERRMEM 3     // Memory error
#define XRAY_ERRTYPE 4    // Type error

// Type tags
#define XRAY_TNONE (-1)
#define XRAY_TNULL 0
#define XRAY_TBOOLEAN 1
#define XRAY_TINTEGER 2
#define XRAY_TNUMBER 3
#define XRAY_TSTRING 4
#define XRAY_TARRAY 5
#define XRAY_TMAP 6
#define XRAY_TFUNCTION 7
#define XRAY_TUSERDATA 8

// Stack indices
#define XRAY_REGISTRYINDEX (-10000)
#define XRAY_GLOBALSINDEX (-10001)

/* ========== VM Management ========== */

/*
** Create a new Xray VM
** Returns NULL on failure
*/
XrayVM *xray_vm_new(void);

/*
** Create a new Xray VM with specified backend
** backend: "vm" (default), "llvm-jit"
*/
XrayVM *xray_vm_new_backend(const char *backend);

/*
** Close VM and free all resources
*/
void xray_vm_close(XrayVM *vm);

/*
** Get Xray version string
*/
const char *xray_version(void);

/* ========== Code Execution ========== */

/*
** Execute a Xray code string
** Returns: XRAY_OK on success, error code otherwise
** Error message is pushed onto the stack top
*/
int xray_vm_dostring(XrayVM *vm, const char *code);

/*
** Execute a Xray file
** Returns: XRAY_OK on success, error code otherwise
*/
int xray_vm_dofile(XrayVM *vm, const char *filename);

/*
** Compile Xray code without executing
** Compiled function is pushed onto the stack top
*/
int xray_vm_compile(XrayVM *vm, const char *code, const char *name);

/* ========== Stack Operations ========== */

/*
** Get top index (number of elements on the stack)
*/
int xray_gettop(XrayVM *vm);

/*
** Set stack top position
** index can be 0 (clear stack) or positive
*/
void xray_settop(XrayVM *vm, int index);

/*
** Push a copy of the element at the given index onto the stack top
*/
void xray_pushvalue(XrayVM *vm, int index);

/*
** Remove a stack element
*/
void xray_remove(XrayVM *vm, int index);

/*
** Insert the top element at the given position
*/
void xray_insert(XrayVM *vm, int index);

/*
** Pop n elements
*/
void xray_pop(XrayVM *vm, int n);

/*
** Check stack space
** Ensures at least n free slots on the stack
*/
int xray_checkstack(XrayVM *vm, int n);

/* ========== Data Exchange: C -> Xray (push to stack) ========== */

/*
** Push null
*/
void xray_pushnull(XrayVM *vm);

/*
** Push a boolean
*/
void xray_pushboolean(XrayVM *vm, int b);

/*
** Push an integer
*/
void xray_pushinteger(XrayVM *vm, int64_t n);

/*
** Push a number
*/
void xray_pushnumber(XrayVM *vm, double n);

/*
** Push a string (copied)
*/
void xray_pushstring(XrayVM *vm, const char *s);

/*
** Push a string with explicit length
*/
void xray_pushlstring(XrayVM *vm, const char *s, size_t len);

/*
** Push a formatted string
*/
const char *xray_pushfstring(XrayVM *vm, const char *fmt, ...);

/*
** Push a C function
*/
void xray_pushcfunction(XrayVM *vm, XrayCFunction f);

/*
** Push light userdata (pointer, not GC-managed)
*/
void xray_pushlightuserdata(XrayVM *vm, void *p);

/* ========== Data Exchange: Xray -> C (get from stack) ========== */

/*
** Get type
*/
int xray_type(XrayVM *vm, int index);
const char *xray_typename(XrayVM *vm, int tp);

/*
** Type checks
*/
int xray_isnull(XrayVM *vm, int index);
int xray_isboolean(XrayVM *vm, int index);
int xray_isinteger(XrayVM *vm, int index);
int xray_isnumber(XrayVM *vm, int index);
int xray_isstring(XrayVM *vm, int index);
int xray_isfunction(XrayVM *vm, int index);
int xray_isuserdata(XrayVM *vm, int index);

/*
** Convert to C type (does not modify stack)
** Returns 0/NULL/false on type mismatch
*/
int xray_toboolean(XrayVM *vm, int index);
int64_t xray_tointeger(XrayVM *vm, int index);
double xray_tonumber(XrayVM *vm, int index);
const char *xray_tostring(XrayVM *vm, int index);
const char *xray_tolstring(XrayVM *vm, int index, size_t *len);
XrayCFunction xray_tocfunction(XrayVM *vm, int index);
void *xray_touserdata(XrayVM *vm, int index);

/* ========== Global Variables and Table Operations ========== */

/*
** Get a global variable and push it onto the stack
*/
void xray_getglobal(XrayVM *vm, const char *name);

/*
** Set a global variable (pops value from stack top)
*/
void xray_setglobal(XrayVM *vm, const char *name);

/*
** Table operation: t[k] = v
** t is at stack index, v is at stack top, k is a string
*/
void xray_setfield(XrayVM *vm, int index, const char *k);

/*
** Table operation: get t[k] and push onto stack
*/
void xray_getfield(XrayVM *vm, int index, const char *k);

/*
** Array operation: t[i] = v
*/
void xray_seti(XrayVM *vm, int index, int64_t i);

/*
** Array operation: get t[i]
*/
void xray_geti(XrayVM *vm, int index, int64_t i);

/*
** Create a new array
*/
void xray_newtable(XrayVM *vm);

/*
** Create a pre-allocated array
*/
void xray_createtable(XrayVM *vm, int narr, int nrec);

/* ========== Function Calls ========== */

/*
** Call a function
** Stack layout: [func] [arg1] [arg2] ... [argN]
** nargs: number of arguments
** nresults: expected number of return values (-1 for all)
*/
int xray_call(XrayVM *vm, int nargs, int nresults);

/*
** Protected call (catches errors)
*/
int xray_pcall(XrayVM *vm, int nargs, int nresults, int errfunc);

/* ========== C Function Registration ========== */

/*
** Register a global C function
** Equivalent to: xray_pushcfunction + xray_setglobal
*/
void xray_register(XrayVM *vm, const char *name, XrayCFunction f);

/*
** Batch register functions
*/
typedef struct XrayReg {
    const char *name;
    XrayCFunction func;
} XrayReg;

void xray_register_lib(XrayVM *vm, const XrayReg *lib);

/* ========== Userdata (C Object Management) ========== */

/*
** Create new userdata
** Returns pointer to userdata memory
** Managed by Xray's GC automatically
*/
void *xray_newuserdata(XrayVM *vm, size_t size);

/*
** Set userdata metatable
*/
int xray_setmetatable(XrayVM *vm, int index);

/*
** Get userdata metatable
*/
int xray_getmetatable(XrayVM *vm, int index);

/*
** Create metatable and register it
** Returns 0 if already exists, 1 otherwise
*/
int xray_newmetatable(XrayVM *vm, const char *tname);

/*
** Check userdata type
** Returns userdata pointer if type matches, otherwise raises error
*/
void *xray_checkudata(XrayVM *vm, int index, const char *tname);

/* ========== Auxiliary Functions (Convenience API) ========== */

/*
** Check argument type, raises error on mismatch
*/
void xray_checktype(XrayVM *vm, int arg, int t);
void xray_checkany(XrayVM *vm, int arg);

int64_t xray_checkinteger(XrayVM *vm, int arg);
double xray_checknumber(XrayVM *vm, int arg);
const char *xray_checkstring(XrayVM *vm, int arg);

/*
** Optional arguments (with defaults)
*/
int64_t xray_optinteger(XrayVM *vm, int arg, int64_t def);
double xray_optnumber(XrayVM *vm, int arg, double def);
const char *xray_optstring(XrayVM *vm, int arg, const char *def);

/*
** Error handling
*/
int xray_error(XrayVM *vm, const char *fmt, ...);

/*
** Argument error
*/
int xray_argerror(XrayVM *vm, int arg, const char *extramsg);

/* ========== Advanced Features ========== */

/*
** Get stack traceback
*/
void xray_traceback(XrayVM *vm, XrayVM *vm1, const char *msg, int level);

/*
** Reference system (persistent references)
*/
int xray_ref(XrayVM *vm, int t);
void xray_unref(XrayVM *vm, int t, int ref);

/*
** GC control
*/
int xray_gc(XrayVM *vm, int what, int data);

#define XRAY_GCSTOP 0
#define XRAY_GCRESTART 1
#define XRAY_GCCOLLECT 2
#define XRAY_GCCOUNT 3
#define XRAY_GCSTEP 4

/* ========== Backend Control ========== */

/*
** Switch backend (if supported)
** backend: "vm", "llvm-jit"
*/
int xray_set_backend(XrayVM *vm, const char *backend);

/*
** Get current backend
*/
const char *xray_get_backend(XrayVM *vm);

/*
** AOT compile to file (LLVM backend)
** target: "x86_64", "arm64", "wasm32", etc.
*/
int xray_compile_to_file(XrayVM *vm, const char *source, const char *output, const char *target);

/* ========== Macros (Convenience) ========== */

// Pop n elements
#define xray_pop(vm, n) xray_settop(vm, -(n) - 1)

// Is none or null
#define xray_isnoneornull(vm, n) (xray_type(vm, (n)) <= 0)

// Register function (convenience macro)
#define xray_register(vm, n, f) (xray_pushcfunction(vm, (f)), xray_setglobal(vm, (n)))

/* ========== Usage Examples ========== */

/*
Example 1: Basic Embedding

    #include <xray/xray_embedding.h>

    int main() {
        XrayVM *vm = xray_vm_new();

        xray_vm_dostring(vm, "print('Hello from Xray')");

        xray_vm_close(vm);
        return 0;
    }

Example 2: Calling Xray Functions

    XrayVM *vm = xray_vm_new();
    xray_vm_dostring(vm, "func add(a: int, b: int) -> int { return a + b }");

    xray_getglobal(vm, "add");
    xray_pushinteger(vm, 10);
    xray_pushinteger(vm, 20);
    xray_call(vm, 2, 1);

    int result = xray_tointeger(vm, -1);
    printf("Result: %d\n", result);  // 30

Example 3: Registering C Functions

    int my_add(XrayVM *vm) {
        int a = xray_checkinteger(vm, 1);
        int b = xray_checkinteger(vm, 2);
        xray_pushinteger(vm, a + b);
        return 1;
    }

    XrayVM *vm = xray_vm_new();
    xray_register(vm, "my_add", my_add);

    xray_vm_dostring(vm, "let sum = my_add(10, 20)");

Example 4: Userdata (C Objects)

    typedef struct { int x, y; } Point;

    int point_new(XrayVM *vm) {
        int x = xray_checkinteger(vm, 1);
        int y = xray_checkinteger(vm, 2);
        Point *p = (Point*)xray_newuserdata(vm, sizeof(Point));
        p->x = x;
        p->y = y;
        xray_getmetatable(vm, "Point");
        xray_setmetatable(vm, -2);
        return 1;
    }

    int point_distance(XrayVM *vm) {
        Point *p = (Point*)xray_checkudata(vm, 1, "Point");
        double dist = sqrt(p->x * p->x + p->y * p->y);
        xray_pushnumber(vm, dist);
        return 1;
    }

    // Register
    xray_register(vm, "Point", point_new);
    xray_register(vm, "point_distance", point_distance);

    // Use in Xray
    xray_vm_dostring(vm, "let p = Point(3, 4); print(point_distance(p))");
*/

#ifdef __cplusplus
}
#endif

#endif  // XRAY_EMBEDDING_H
