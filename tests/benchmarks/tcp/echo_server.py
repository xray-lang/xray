#!/usr/bin/env python3
"""TCP benchmark server for Python (asyncio)."""

import asyncio
import sys

CHUNK = b"S" * 65536


async def serve_message(reader, writer):
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


async def serve_discard(reader, writer):
    try:
        while True:
            data = await reader.read(65536)
            if not data:
                break
        writer.write(b"OK")
        await writer.drain()
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def serve_slow_discard(reader, writer, delay_ms):
    delay = max(0, delay_ms) / 1000.0
    try:
        while True:
            data = await reader.read(4096)
            if not data:
                break
            if delay > 0:
                await asyncio.sleep(delay)
        writer.write(b"OK")
        await writer.drain()
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def serve_source(reader, writer, total_bytes):
    remaining = total_bytes
    try:
        while remaining > 0:
            data = CHUNK if remaining >= len(CHUNK) else CHUNK[:remaining]
            writer.write(data)
            await writer.drain()
            remaining -= len(data)
    except (ConnectionResetError, BrokenPipeError):
        pass
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass


async def copy_stream(reader, writer):
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


async def serve_proxy(reader, writer, upstream_port):
    try:
        upstream_reader, upstream_writer = await asyncio.open_connection("127.0.0.1", upstream_port)
    except OSError:
        writer.close()
        await writer.wait_closed()
        return

    await asyncio.gather(
        copy_stream(reader, upstream_writer),
        copy_stream(upstream_reader, writer),
        return_exceptions=True,
    )


async def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 9001
    mode = sys.argv[2] if len(sys.argv) > 2 else "message"
    extra_arg = sys.argv[3] if len(sys.argv) > 3 and sys.argv[3] else None
    extra = int(extra_arg) if extra_arg is not None else 64 * 1024 * 1024

    async def handle_client(reader, writer):
        if mode == "discard":
            await serve_discard(reader, writer)
        elif mode == "slow_discard":
            await serve_slow_discard(reader, writer, extra)
        elif mode == "source":
            await serve_source(reader, writer, extra)
        elif mode == "proxy":
            await serve_proxy(reader, writer, extra)
        else:
            await serve_message(reader, writer)

    server = await asyncio.start_server(handle_client, "0.0.0.0", port)
    print(f"Python TCP benchmark server listening on port {port} mode={mode}")
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
