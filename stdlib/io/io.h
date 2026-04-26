/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * io.h - File I/O standard library
 *
 * KEY CONCEPT:
 *   Provides file read/write and directory operations. All calls are
 *   synchronous at the moment; a future revision will offload blocking
 *   filesystem syscalls to XrAsyncPool.
 */

#ifndef XR_STDLIB_IO_H
#define XR_STDLIB_IO_H

#include "../../src/module/xmodule.h"
#include "../../src/vm/xvm.h"

/*
 * Load io module
 *
 * File read/write:
 *   - readFile(path)              Read file content as string
 *   - readFileBytes(path)         Read file as byte array (Array<uint8>)
 *   - writeFile(path, content)    Write string to file
 *   - writeFileBytes(path, bytes) Write byte array to file
 *   - appendFile(path, content)   Append string to file
 *   - copyFile(src, dst)          Copy file
 *   - readLines(path)             Read file by lines
 *
 * File checks:
 *   - exists(path)                Check if path exists
 *   - isFile(path)                Check if path is a file
 *   - isDir(path)                 Check if path is a directory
 *   - isSymlink(path)             Check if path is a symlink
 *   - fileSize(path)              Get file size
 *   - stat(path)                  Get file stat info
 *
 * File operations:
 *   - remove(path)                Remove file
 *   - removeAll(path)             Recursively remove directory
 *   - rename(old, new)            Rename/move file
 *   - chmod(path, mode)           Change file permissions
 *   - touch(path)                 Create empty file or update timestamp
 *
 * Directory operations:
 *   - mkdir(path)                 Create directory
 *   - mkdirp(path)                Recursively create directory
 *   - readDir(path)               Read directory contents
 *   - readDirRecursive(path)      Recursively read directory
 *   - cwd()                       Get current working directory
 *   - chdir(path)                 Change working directory
 *
 * Symbolic links:
 *   - symlink(target, path)       Create symbolic link
 *   - readlink(path)              Read symbolic link target
 *   - realpath(path)              Get resolved absolute path
 *
 * Temporary files:
 *   - tempFile()                  Create temporary file
 *   - tempDir()                   Create temporary directory
 */
XrModule *xr_load_module_io(XrayIsolate *isolate);

#endif
