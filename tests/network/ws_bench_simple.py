#!/usr/bin/env python3
"""Simple WebSocket benchmark - sequential connections"""

import asyncio
import websockets
import time
import sys

async def run_benchmark(url, num_messages=1000, message_size=128):
    print(f"\n=== Simple WebSocket Benchmark ===")
    print(f"URL: {url}")
    print(f"Messages: {num_messages}")
    print(f"Message size: {message_size} bytes")
    
    msg = "X" * message_size
    latencies = []
    
    try:
        start = time.perf_counter()
        async with websockets.connect(url) as ws:
            connect_time = (time.perf_counter() - start) * 1000
            print(f"Connected in {connect_time:.2f}ms")
            
            for i in range(num_messages):
                send_start = time.perf_counter()
                await ws.send(msg)
                response = await ws.recv()
                latency = (time.perf_counter() - send_start) * 1000
                latencies.append(latency)
                
                if (i + 1) % 200 == 0:
                    print(f"  Progress: {i+1}/{num_messages}")
            
            total_time = time.perf_counter() - start
            
            print(f"\n=== Results ===")
            print(f"Total time: {total_time:.2f}s")
            print(f"Messages: {num_messages}")
            print(f"Throughput: {num_messages/total_time:.0f} msg/s")
            print(f"Latency (ms):")
            print(f"  Min: {min(latencies):.2f}")
            print(f"  Max: {max(latencies):.2f}")
            print(f"  Avg: {sum(latencies)/len(latencies):.2f}")
            latencies.sort()
            print(f"  P95: {latencies[int(len(latencies)*0.95)]:.2f}")
            print(f"  P99: {latencies[int(len(latencies)*0.99)]:.2f}")
            
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    url = sys.argv[1] if len(sys.argv) > 1 else "ws://localhost:8765"
    asyncio.run(run_benchmark(url))
