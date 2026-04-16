#!/usr/bin/env python3
"""
WebSocket Performance Benchmark Tool

Usage:
  1. Start xray WebSocket server:
     ./build-release/xray tests/network/ws_server_test.xr
  
  2. Run this benchmark:
     python3 tests/network/ws_bench.py ws://localhost:8765

Requirements:
  pip3 install websockets
"""

import asyncio
import websockets
import time
import sys
import statistics

# Configuration
DEFAULT_URL = "ws://localhost:8765"
NUM_CONNECTIONS = 20       # Number of concurrent connections
MESSAGES_PER_CONN = 50     # Messages per connection
MESSAGE_SIZE = 128         # Message size in bytes

async def benchmark_single_connection(url, conn_id, results):
    """Benchmark a single WebSocket connection"""
    try:
        start = time.perf_counter()
        async with websockets.connect(url) as ws:
            connect_time = time.perf_counter() - start
            
            msg = "X" * MESSAGE_SIZE
            latencies = []
            
            for i in range(MESSAGES_PER_CONN):
                send_start = time.perf_counter()
                await ws.send(msg)
                response = await ws.recv()
                latency = (time.perf_counter() - send_start) * 1000  # ms
                latencies.append(latency)
            
            results[conn_id] = {
                'connect_time': connect_time * 1000,
                'latencies': latencies,
                'messages': MESSAGES_PER_CONN,
                'success': True
            }
    except Exception as e:
        results[conn_id] = {
            'success': False,
            'error': str(e)
        }

async def run_benchmark(url):
    """Run the full benchmark"""
    print(f"\n=== WebSocket Performance Benchmark ===")
    print(f"URL: {url}")
    print(f"Concurrent connections: {NUM_CONNECTIONS}")
    print(f"Messages per connection: {MESSAGES_PER_CONN}")
    print(f"Message size: {MESSAGE_SIZE} bytes")
    print(f"Total messages: {NUM_CONNECTIONS * MESSAGES_PER_CONN}")
    print()
    
    results = {}
    
    # Warm up
    print("Warming up...")
    warmup_result = {}
    await benchmark_single_connection(url, -1, warmup_result)
    if warmup_result.get(-1, {}).get('success'):
        print("Warmup successful")
    else:
        print(f"Warmup failed: {warmup_result.get(-1, {}).get('error', 'unknown')}")
        return
    
    # Run benchmark with staggered connections
    print("Running benchmark...")
    start_time = time.perf_counter()
    
    # Create connections in batches to avoid overwhelming server
    BATCH_SIZE = 5
    tasks = []
    for i in range(NUM_CONNECTIONS):
        tasks.append(benchmark_single_connection(url, i, results))
        if (i + 1) % BATCH_SIZE == 0:
            await asyncio.sleep(0.01)  # Small delay between batches
    
    await asyncio.gather(*tasks)
    
    total_time = time.perf_counter() - start_time
    
    # Analyze results
    successful = [r for r in results.values() if r.get('success')]
    failed = [r for r in results.values() if not r.get('success')]
    
    print(f"\n=== Results ===")
    print(f"Total time: {total_time:.2f}s")
    print(f"Successful connections: {len(successful)}/{NUM_CONNECTIONS}")
    
    if failed:
        print(f"Failed connections: {len(failed)}")
        for r in failed[:3]:  # Show first 3 errors
            print(f"  Error: {r.get('error', 'unknown')}")
    
    if successful:
        # Connection time stats
        connect_times = [r['connect_time'] for r in successful]
        print(f"\nConnection time:")
        print(f"  Min: {min(connect_times):.2f}ms")
        print(f"  Max: {max(connect_times):.2f}ms")
        print(f"  Avg: {statistics.mean(connect_times):.2f}ms")
        
        # Latency stats
        all_latencies = []
        for r in successful:
            all_latencies.extend(r['latencies'])
        
        total_messages = len(all_latencies)
        throughput = total_messages / total_time
        
        print(f"\nMessage latency:")
        print(f"  Min: {min(all_latencies):.2f}ms")
        print(f"  Max: {max(all_latencies):.2f}ms")
        print(f"  Avg: {statistics.mean(all_latencies):.2f}ms")
        print(f"  Median: {statistics.median(all_latencies):.2f}ms")
        print(f"  P95: {sorted(all_latencies)[int(len(all_latencies)*0.95)]:.2f}ms")
        print(f"  P99: {sorted(all_latencies)[int(len(all_latencies)*0.99)]:.2f}ms")
        
        print(f"\nThroughput:")
        print(f"  Messages/sec: {throughput:.0f}")
        print(f"  MB/sec: {throughput * MESSAGE_SIZE / 1024 / 1024:.2f}")

def main():
    url = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_URL
    
    try:
        import websockets
    except ImportError:
        print("Please install websockets: pip3 install websockets")
        sys.exit(1)
    
    asyncio.run(run_benchmark(url))

if __name__ == "__main__":
    main()
