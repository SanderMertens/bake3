#/bin/sh

# Build bake
make clean
make

# Clean all test projects
./build/bake clean

# Build test projects
./build/bake test/projects

# Build integration tests
./build/bake test/integration

# Build projects and integration from test root
./build/bake clean
./build/bake test

# Build module folder directly
./build/bake clean
./build/bake test/integration/flecs-module-test
