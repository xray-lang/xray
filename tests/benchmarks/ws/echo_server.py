#!/usr/bin/env python3
"""
echo_server.py - Python WebSocket echo server (websockets) for benchmark.

Usage:
  python3 echo_server.py [port]

Default port: 9001
Requires: pip3 install websockets
"""

import asyncio
import sys

import websockets


async def echo(websocket):
    async for message in websocket:
        await websocket.send(message)


async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9001
    print(f"Python WebSocket Echo Server (websockets) on port {port}")
    async with websockets.serve(
        echo, "127.0.0.1", port, ping_interval=None, max_size=10 * 1024 * 1024
    ):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
