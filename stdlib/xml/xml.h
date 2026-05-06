/*
 * xray - Lightweight typed scripting with native concurrency
 * https://www.xray-lang.org
 *
 * Copyright (c) 2026 Xinglei Xu <xingleixu@gmail.com>
 * Licensed under the MIT License
 *
 * xml.h - XML standard library module interface
 *
 * KEY CONCEPT:
 *   High-performance XML library based on state machine parsing.
 *   Configuration uses Json objects.
 *
 * API:
 *   Parse:
 *     - parse(str)                Parse XML string
 *     - parse(str, config)        Parse with configuration
 *     - parseFile(path)           Parse XML file
 *     - parseDetailed(str)        Return detailed result with errors
 *   Serialize:
 *     - stringify(node)           Serialize to XML string
 *     - stringify(node, config)   Serialize with configuration
 *     - writeFile(path, node)     Write to XML file
 *   Build:
 *     - document()                Create empty document
 *     - element(tag)              Create element node
 *     - element(tag, attrs)       Create element with attributes
 *     - text(content)             Create text node
 *     - comment(content)          Create comment node
 *     - cdata(content)            Create CDATA node
 *
 * CONFIG OPTIONS (Json object):
 *   Parse config:
 *     - preserveWhitespace: bool    Preserve whitespace text nodes
 *     - preserveComments: bool      Preserve comments
 *     - preserveCData: bool         Preserve CDATA
 *   Serialize config:
 *     - indent: int                 Indent spaces (0 = compact)
 *     - declaration: bool           Add XML declaration
 *     - encoding: string            Encoding declaration
 */

#ifndef XR_STDLIB_XML_H
#define XR_STDLIB_XML_H

#include "../../src/base/xdefs.h"

struct XrayIsolate;
struct XrModule;

XR_FUNC struct XrModule *xr_load_module_xml(struct XrayIsolate *isolate);

#endif  // XR_STDLIB_XML_H
