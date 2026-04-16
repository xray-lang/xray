#!/usr/bin/env python3
"""
Python WebSocket echo server for testing xray client.
Default port: 9101
"""

import asyncio
import sys
import websockets


async def echo(websocket):
    async for message in websocket:
        await websocket.send(message)


async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9101
    print(f"Python WebSocket Echo Server on port {port}")
    async with websockets.serve(
        echo, "127.0.0.1", port, ping_interval=None, max_size=10 * 1024 * 1024,
        compression=None
    ):
        await asyncio.Future()


if __name__ == "__main__":
    asyncio.run(main())
