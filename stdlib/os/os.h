/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * os.h - Operating system interface module
 *
 * KEY CONCEPT:
 *   Provides OS-level operations: environment variables, process control,
 *   and platform information.
 */

#ifndef XR_STDLIB_OS_H
#define XR_STDLIB_OS_H

#include "../../src/base/xdefs.h"

struct XrayIsolate;
struct XrModule;

/*
 * Environment variables:
 *   getenv(name)         - Get environment variable
 *   setenv(name, value)  - Set environment variable
 *   unsetenv(name)       - Delete environment variable
 *   environ()            - Get all environment variables (returns Map)
 *
 * Process control:
 *   exit(code)           - Exit program
 *   getpid()             - Get process ID
 *   ppid()               - Get parent process ID
 *   getcwd()             - Get current working directory
 *   chdir(path)          - Change working directory
 *   hostname()           - Get hostname
 *   tmpdir()             - Get temporary directory
 *   exec(cmd)            - Execute shell command
 *   kill(pid, signal)    - Send signal to process
 *   sleep(ms)            - Sleep for milliseconds
 *   clock()              - Get process CPU time
 *
 * User information:
 *   username()           - Get current user name
 *   homedir()            - Get user home directory
 *   uid()                - Get user ID
 *   gid()                - Get group ID
 *
 * System information:
 *   cpuCount()           - Get number of CPU cores
 *   totalMemory()        - Get total system memory
 *   freeMemory()         - Get available system memory
 *   uptime()             - Get system uptime
 *   loadavg()            - Get system load averages
 *
 * Constants:
 *   platform             - OS name ("darwin"/"linux"/"windows")
 *   arch                 - CPU architecture ("arm64"/"x64"/"x86")
 *   sep                  - Path separator ("/" or "\\")
 *   eol                  - Line ending ("\n" or "\r\n")
 */
XR_FUNC struct XrModule *xr_load_module_os(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_OS_H
