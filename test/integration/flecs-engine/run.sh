#!/usr/bin/env sh
set -eu

if [ ! -f build/CMakeCache.txt ]; then
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
fi
cmake --build build --parallel 8
./build/flecs_engine "$@"
