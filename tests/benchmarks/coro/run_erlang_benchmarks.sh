#!/bin/bash
# Run all Erlang benchmarks
# Usage: bash tests/coro_benchmark/run_erlang_benchmarks.sh

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

echo "========================================"
echo "  Erlang Benchmark Suite"
echo "========================================"
echo ""

# Ring
echo "--- Ring 1K×1K ---"
cd ring
erlc ring.erl && erl -noshell -s ring main -s init stop
cd ..
echo ""

# Pingpong
echo "--- Pingpong 1M ---"
cd pingpong
erlc pingpong.erl && erl -noshell -s pingpong main -s init stop
cd ..
echo ""

# Skynet
echo "--- Skynet depth=6 ---"
cd skynet
erlc skynet.erl && erl -noshell -s skynet main -s init stop
cd ..
echo ""

# Spawn
echo "--- Spawn 1M ---"
cd spawn
erlc spawn.erl && erl -noshell -s spawn main -s init stop
cd ..
echo ""

# Producer-Consumer
echo "--- Producer-Consumer 10×10×100K ---"
cd producer_consumer
erlc producer_consumer.erl && erl -noshell -s producer_consumer main -s init stop
cd ..
echo ""

# Fanout
echo "--- Fanout 1K×100K ---"
cd fanout
erlc fanout.erl && erl -noshell -s fanout main -s init stop
cd ..
echo ""

# Parallel Sum
echo "--- Parallel Sum 8×10M ---"
cd parallel_sum
erlc parallel_sum.erl && erl -noshell -s parallel_sum main -s init stop
cd ..
echo ""

# Sleep Storm
echo "--- Sleep Storm 10K ---"
cd sleep_storm
erlc sleep_storm.erl && erl -noshell -s sleep_storm main -s init stop
cd ..
echo ""

echo "========================================"
echo "  Done"
echo "========================================"
