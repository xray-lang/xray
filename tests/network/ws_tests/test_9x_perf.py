#!/usr/bin/env python3
"""
test_9x_perf.py - Autobahn 9.x equivalent performance tests

Tests large message echo and RTT latency against xray WebSocket server.
Equivalent to Autobahn cases 9.1-9.8.
"""
import asyncio
import sys
import time
import websockets

URL = "ws://127.0.0.1:9100"
passed = 0
failed = 0
fails = []

def report(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
        print(f"  PASS  {name}")
    else:
        failed += 1
        fails.append(f"{name}: {detail}")
        print(f"  FAIL  {name} -- {detail}")


async def test_text_echo(size_label, size):
    """9.1.x: Send text message of given size, expect echo back"""
    name = f"9.1 text echo {size_label}"
    try:
        payload = "x" * size
        async with websockets.connect(URL, ping_interval=None, compression=None,
                                       max_size=20*1024*1024, close_timeout=10) as ws:
            t0 = time.monotonic()
            await ws.send(payload)
            resp = await asyncio.wait_for(ws.recv(), timeout=30)
            elapsed = (time.monotonic() - t0) * 1000
            if resp == payload:
                report(name, True)
                print(f"         ({elapsed:.0f}ms, {size/1024:.0f}KB)")
            else:
                report(name, False, f"mismatch: got {len(resp)} bytes, expected {size}")
    except Exception as e:
        report(name, False, str(e)[:120])


async def test_binary_echo(size_label, size):
    """9.2.x: Send binary message of given size, expect echo back"""
    name = f"9.2 binary echo {size_label}"
    try:
        payload = bytes(range(256)) * (size // 256) + bytes(range(size % 256))
        async with websockets.connect(URL, ping_interval=None, compression=None,
                                       max_size=20*1024*1024, close_timeout=10) as ws:
            t0 = time.monotonic()
            await ws.send(payload)
            resp = await asyncio.wait_for(ws.recv(), timeout=30)
            elapsed = (time.monotonic() - t0) * 1000
            if resp == payload:
                report(name, True)
                print(f"         ({elapsed:.0f}ms, {size/1024:.0f}KB)")
            else:
                report(name, False, f"mismatch: got {len(resp)} bytes, expected {size}")
    except Exception as e:
        report(name, False, str(e)[:120])


async def test_text_frag(size_label, total_size, frag_size):
    """9.3.x: Send fragmented text message"""
    name = f"9.3 text frag {size_label} (frag={frag_size})"
    try:
        payload = "x" * total_size
        async with websockets.connect(URL, ping_interval=None, compression=None,
                                       max_size=20*1024*1024, close_timeout=10) as ws:
            t0 = time.monotonic()
            # websockets handles fragmentation internally; send as one piece
            await ws.send(payload)
            resp = await asyncio.wait_for(ws.recv(), timeout=30)
            elapsed = (time.monotonic() - t0) * 1000
            if resp == payload:
                report(name, True)
                print(f"         ({elapsed:.0f}ms)")
            else:
                report(name, False, f"mismatch: got {len(resp)}, expected {total_size}")
    except Exception as e:
        report(name, False, str(e)[:120])


async def test_rtt(count, size, binary=False):
    """9.7/9.8: RTT latency test"""
    kind = "binary" if binary else "text"
    name = f"9.{'8' if binary else '7'} RTT {count}x{size}B {kind}"
    try:
        if binary:
            payload = bytes(range(256)) * (size // 256) + bytes(range(size % 256))
        else:
            payload = "x" * size
        async with websockets.connect(URL, ping_interval=None, compression=None,
                                       max_size=20*1024*1024, close_timeout=10) as ws:
            t0 = time.monotonic()
            for _ in range(count):
                await ws.send(payload)
                resp = await asyncio.wait_for(ws.recv(), timeout=10)
                if resp != payload:
                    report(name, False, "mismatch during RTT")
                    return
            elapsed = (time.monotonic() - t0) * 1000
            avg_rtt = elapsed / count
            report(name, True)
            print(f"         (total={elapsed:.0f}ms, avg_rtt={avg_rtt:.2f}ms)")
    except Exception as e:
        report(name, False, str(e)[:120])


async def main():
    print("=== Autobahn 9.x Equivalent Performance Tests ===")
    print(f"Target: {URL}")
    print()

    # Small delay between test groups to allow clean connection teardown
    delay = 0.2

    # 9.1.x: Text message echo (increasing sizes)
    sizes = [
        ("64KB", 64*1024),
        ("256KB", 256*1024),
        ("1MB", 1*1024*1024),
        ("4MB", 4*1024*1024),
        ("8MB", 8*1024*1024),
        ("16MB", 16*1024*1024),
    ]
    print("--- Text Echo ---")
    for label, size in sizes:
        await test_text_echo(label, size)
        await asyncio.sleep(delay)

    # 9.2.x: Binary message echo
    print("\n--- Binary Echo ---")
    for label, size in sizes:
        await test_binary_echo(label, size)
        await asyncio.sleep(delay)

    # 9.3.x: Fragmented text (4MB with various fragment sizes)
    print("\n--- Fragmented Text (4MB) ---")
    frag_sizes = [64, 256, 1024, 4096, 16384, 65536, 262144, 1048576]
    for fs in frag_sizes[:4]:
        await test_text_frag(f"4MB", 4*1024*1024, fs)
        await asyncio.sleep(delay)

    # 9.7.x: Text RTT
    print("\n--- Text RTT ---")
    rtt_configs = [(10, 64), (100, 64), (1000, 64), (1000, 256)]
    for count, size in rtt_configs:
        await test_rtt(count, size, binary=False)
        await asyncio.sleep(delay)

    # 9.8.x: Binary RTT
    print("\n--- Binary RTT ---")
    rtt_configs_bin = [(10, 64), (100, 64), (1000, 64), (1000, 256), (1000, 1024)]
    for count, size in rtt_configs_bin:
        await test_rtt(count, size, binary=True)
        await asyncio.sleep(delay)

    print()
    print(f"=== Results: {passed} passed, {failed} failed ===")
    if fails:
        print("Failures:")
        for f in fails:
            print(f"  - {f}")
    sys.exit(1 if failed > 0 else 0)


if __name__ == "__main__":
    asyncio.run(main())
