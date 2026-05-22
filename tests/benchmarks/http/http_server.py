#!/usr/bin/env python3
"""
http_server.py - Python HTTP server (aiohttp) for benchmark.

Usage:
  python3 http_server.py [port]

Default port: 8080
Requires: pip3 install aiohttp
"""

import asyncio
import sys

from aiohttp import web

PLAIN_RESP = b"Hello, World!"
JSON_RESP = b'{"message":"Hello, World!"}'


async def plaintext_handler(request):
    return web.Response(body=PLAIN_RESP, content_type="text/plain")


async def json_handler(request):
    return web.Response(body=JSON_RESP, content_type="application/json")


async def echo_handler(request):
    body = await request.read()
    return web.Response(body=body, content_type="application/octet-stream")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"Python HTTP Server (aiohttp) on port {port}")

    app = web.Application()
    app.router.add_get("/plaintext", plaintext_handler)
    app.router.add_get("/json", json_handler)
    app.router.add_post("/echo", echo_handler)

    web.run_app(app, host="127.0.0.1", port=port, print=None,
                access_log=None)


if __name__ == "__main__":
    main()
