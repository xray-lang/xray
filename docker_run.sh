#!/bin/bash
set -e
echo "=== cmake configure ==="
cmake -B build-docker -DCMAKE_BUILD_TYPE=Release
echo "=== cmake build ==="
cmake --build build-docker -j4
echo "=== build done ==="

echo ""
echo "--- crash_min ---"
./build-docker/xray test tests/regression/10_stdlib/crash_min.xr 2>&1
echo "EXIT=$?"

echo ""
echo "--- 0521_cell_upval ---"
./build-docker/xray test tests/regression/05_functions/0521_cell_upval.xr 2>&1
echo "EXIT=$?"

echo ""
echo "--- 0881_nested_iterator_combo ---"
./build-docker/xray test tests/regression/08_oop/0881_nested_iterator_combo.xr 2>&1
echo "EXIT=$?"
