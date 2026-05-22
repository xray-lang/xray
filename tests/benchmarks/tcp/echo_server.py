#!/usr/bin/env python3
"""TCP Echo Server for Python benchmark (asyncio)."""

import asyncio
import sys


async def handle_client(reader, writer):
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
            writer.write(data)
            await writer.drain()
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9001
    server = await asyncio.start_server(handle_client, "0.0.0.0", port)
    print(f"Python TCP echo server listening on port {port}")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
