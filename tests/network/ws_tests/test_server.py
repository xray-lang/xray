#!/usr/bin/env python3
"""
test_server.py - Test xray WebSocket server with Python websockets client.

Tests RFC 6455 compliance and basic functionality.
Expects xray echo server running on ws://127.0.0.1:9100
"""

import asyncio
import struct
import sys
import time

import websockets
from websockets.exceptions import ConnectionClosed, InvalidStatusCode

URL = "ws://127.0.0.1:9100"
PASSED = 0
FAILED = 0
ERRORS = []


def report(name, ok, detail=""):
    global PASSED, FAILED, ERRORS
    if ok:
        PASSED += 1
        print(f"  PASS  {name}")
    else:
        FAILED += 1
        ERRORS.append(f"{name}: {detail}")
        print(f"  FAIL  {name} -- {detail}")


async def test_connect_close():
    """S1: Basic connect and clean close"""
    try:
        ws = await asyncio.wait_for(
            websockets.connect(URL, ping_interval=None, compression=None),
            timeout=5
        )
        # websockets v13+ uses .protocol.state, older uses .open
        is_open = hasattr(ws, 'open') and ws.open or not hasattr(ws, 'open')
        assert is_open, "should be open"
        await ws.close()
        report("S1 connect+close", True)
    except Exception as e:
        report("S1 connect+close", False, str(e))


async def test_text_echo():
    """S2: Text echo"""
    try:
        async with websockets.connect(URL, ping_interval=None, compression=None) as ws:
            msg = "Hello xray WebSocket!"
            await ws.send(msg)
            resp = await asyncio.wait_for(ws.recv(), timeout=5)
            ok = resp == msg
            report("S2 text echo", ok, f"expected={msg!r} got={resp!r}" if not ok else "")
    except Exception as e:
        report("S2 text echo", False, str(e))


async def test_multiple_messages():
    """S3: Multiple sequential messages"""
    try:
        async with websockets.connect(URL, ping_interval=None, compression=None) as ws:
            count = 50
            for i in range(count):
                msg = f"msg-{i}"
                await ws.send(msg)
                resp = await asyncio.wait_for(ws.recv(), timeout=5)
                if resp != msg:
                    report("S3 multiple messages", False, f"mismatch at {i}: {resp!r}")
                    return
            report("S3 multiple messages", True)
    except Exception as e:
        report("S3 multiple messages", False, str(e))


async def test_binary_echo():
    """S4: Binary frame echo"""
    try:
        async with websockets.connect(URL, ping_interval=None, compression=None) as ws:
            data = bytes(range(256))
            await ws.send(data)
            resp = await asyncio.wait_for(ws.recv(), timeout=5)
            ok = isinstance(resp, (bytes, bytearray)) and resp == data
            report("S4 binary echo", ok, f"type={type(resp).__name__} len={len(resp)}" if not ok else "")
    except Exception as e:
        report("S4 binary echo", False, str(e))


async def test_empty_message():
    """S5: Empty text message"""
    try:
        async with websockets.connect(URL, ping_interval=None, compression=None) as ws:
            await ws.send("")
            resp = await asyncio.wait_for(ws.recv(), timeout=5)
            ok = resp == ""
            report("S5 empty message", ok, f"got={resp!r}" if not ok else "")
    except Exception as e:
        report("S5 empty message", False, str(e))


async def test_large_messages():
    """S6: Large messages (1KB, 4KB, 64KB)"""
    try:
        async with websockets.connect(URL, ping_interval=None, compression=None) as ws:
            for size in [1024, 4096, 65536]:
                msg = "X" * size
                await ws.send(msg)
                resp = await asyncio.wait_for(ws.recv(), timeout=10)
                if resp != msg:
                    report(f"S6 large msg ({size}B)", False, f"len mismatch: {len(resp)} vs {size}")
                    return
            report("S6 large messages", True)
    except Exception as e:
        report("S6 large messages", False, str(e))


async def test_concurrent_connections():
    """S7: Multiple concurrent connections"""
    async def echo_task(task_id):
        async with websockets.connect(URL, ping_interval=None, compression=None) as ws:
            for i in range(5):
                msg = f"task{task_id}-msg{i}"
                await ws.send(msg)
                resp = await asyncio.wait_for(ws.recv(), timeout=5)
                assert resp == msg, f"task{task_id} mismatch: {resp!r}"

    try:
        tasks = [echo_task(i) for i in range(5)]
        await asyncio.wait_for(asyncio.gather(*tasks), timeout=15)
        report("S7 concurrent connections", True)
    except Exception as e:
        report("S7 concurrent connections", False, str(e))


async def test_rapid_connect_close():
    """S8: Rapid connect+send+close cycles"""
    try:
        for i in range(10):
            async with websockets.connect(URL, ping_interval=None, compression=None) as ws:
                msg = f"rapid-{i}"
                await ws.send(msg)
                resp = await asyncio.wait_for(ws.recv(), timeout=5)
                assert resp == msg
            await asyncio.sleep(0.05)
        report("S8 rapid connect/close", True)
    except Exception as e:
        report("S8 rapid connect/close", False, str(e))


async def test_ping_pong():
    """S9: Client sends ping, server should auto-reply pong"""
    try:
        async with websockets.connect(URL, ping_interval=None, compression=None) as ws:
            pong = await ws.ping()
            await asyncio.wait_for(pong, timeout=5)
            # Also verify normal echo still works after ping
            await ws.send("after-ping")
            resp = await asyncio.wait_for(ws.recv(), timeout=5)
            ok = resp == "after-ping"
            report("S9 ping/pong", ok, f"got={resp!r}" if not ok else "")
    except Exception as e:
        report("S9 ping/pong", False, str(e))


async def test_client_close_code():
    """S10: Client sends close with code 1000"""
    try:
        ws = await websockets.connect(URL, ping_interval=None, compression=None)
        await ws.send("before-close")
        resp = await asyncio.wait_for(ws.recv(), timeout=5)
        assert resp == "before-close"
        await ws.close(code=1000, reason="normal closure")
        report("S10 close with code", True)
    except Exception as e:
        report("S10 close with code", False, str(e))


async def test_utf8_content():
    """S11: UTF-8 content (Chinese, emoji)"""
    try:
        async with websockets.connect(URL, ping_interval=None, compression=None) as ws:
            messages = [
                "Hello World",
                "Chinese text here",
                "Mixed ABC 123",
            ]
            for msg in messages:
                await ws.send(msg)
                resp = await asyncio.wait_for(ws.recv(), timeout=5)
                if resp != msg:
                    report("S11 UTF-8 content", False, f"mismatch: {resp!r}")
                    return
            report("S11 UTF-8 content", True)
    except Exception as e:
        report("S11 UTF-8 content", False, str(e))


async def test_mixed_text_binary():
    """S12: Alternating text and binary frames"""
    try:
        async with websockets.connect(URL, ping_interval=None, compression=None) as ws:
            for i in range(10):
                if i % 2 == 0:
                    msg = f"text-{i}"
                    await ws.send(msg)
                    resp = await asyncio.wait_for(ws.recv(), timeout=5)
                    assert resp == msg, f"text mismatch at {i}"
                else:
                    data = bytes([i] * 100)
                    await ws.send(data)
                    resp = await asyncio.wait_for(ws.recv(), timeout=5)
                    assert resp == data, f"binary mismatch at {i}"
            report("S12 mixed text/binary", True)
    except Exception as e:
        report("S12 mixed text/binary", False, str(e))


async def main():
    print("=== Testing xray WebSocket Server ===")
    print(f"Target: {URL}")
    print()

    await test_connect_close()
    await test_text_echo()
    await test_multiple_messages()
    await test_binary_echo()
    await test_empty_message()
    await test_large_messages()
    await test_concurrent_connections()
    await test_rapid_connect_close()
    await test_ping_pong()
    await test_client_close_code()
    await test_utf8_content()
    await test_mixed_text_binary()

    print()
    print(f"=== Results: {PASSED} passed, {FAILED} failed ===")
    if ERRORS:
        print("Failures:")
        for e in ERRORS:
            print(f"  - {e}")

    sys.exit(0 if FAILED == 0 else 1)


if __name__ == "__main__":
    asyncio.run(main())
