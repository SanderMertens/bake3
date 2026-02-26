#!/bin/sh

# Build bake
make clean
make -j 8

export BAKE_HOME="$(pwd)/test/tmp/bake_home"

echo
echo "Install bake"
./build/bake setup --local

# ---
echo
echo "Reset environment"
./build/bake clean test
./build/bake reset
echo "Listing (should be empty)"
./build/bake list
echo

# ---
echo
echo "Build test"
./build/bake build test
echo "Listing (should show flecs + modules)"
./build/bake list
echo

# ---
echo
echo "Reset environment"
./build/bake clean test
./build/bake reset
echo "Listing (should be empty)"
./build/bake list
echo

# ---
echo
echo "Build test/projects"
./build/bake build test/projects
echo "Listing (should be empty)"
./build/bake list
echo

# ---
echo
echo "Reset environment"
./build/bake clean test
./build/bake reset
echo "Listing (should be empty)"
./build/bake list
echo

# ---
echo
echo "Build test/integration"
./build/bake build test/integration
echo "Listing (should show flecs + modules)"
./build/bake list
echo

# ---
echo
echo "Reset environment"
./build/bake clean test
./build/bake reset
echo "Listing (should be empty)"
./build/bake list
echo

# ---
echo
echo "Build test/integration/flecs-modules-test"
./build/bake build test/integration/flecs-modules-test
echo "Listing (should show flecs + modules)"
./build/bake list
echo

# ---
echo
echo "Rebuild test/integration/flecs-modules-test/apps/city"
./build/bake rebuild test/integration/flecs-modules-test/apps/city
echo

# ---
echo
echo "Rebuild test/integration/flecs-modules-test/apps/tower_defense"
./build/bake rebuild test/integration/flecs-modules-test/apps/tower_defense
echo

# ---
echo
echo "Incrementally build test/integration/flecs-modules-test/apps/city (shouldn't build anything)"
./build/bake build test/integration/flecs-modules-test/apps/city
echo

# ---
echo
echo "Incrementally build test/integration/flecs-modules-test/apps/tower_defens (shouldn't build anything)"
./build/bake build test/integration/flecs-modules-test/apps/tower_defense
echo

# ---
echo
echo "Builds relative to test"
cd test

# ---
echo
echo "Rebuild integration/flecs-modules-test/apps/city"
../build/bake rebuild integration/flecs-modules-test/apps/city
echo

# ---
echo
echo "Rebuild integration/flecs-modules-test/apps/tower_defense"
../build/bake rebuild integration/flecs-modules-test/apps/tower_defense
echo

# ---
echo
echo "Builds relative to integration"
cd integration

# ---
echo
echo "Rebuild flecs-modules-test/apps/city"
../../build/bake rebuild flecs-modules-test/apps/city
echo

# ---
echo
echo "Rebuild flecs-modules-test/apps/tower_defense"
../../build/bake rebuild flecs-modules-test/apps/tower_defense
echo

echo
echo "Listing (should show flecs + modules)"
../../build/bake list

# ---
cd ../..

# ---
echo
echo "Test running test/integration/flecs-modules-test/flecs/test/core"
./build/bake run test/integration/flecs-modules-test/flecs/test/core -- -j 12

# ---
echo
echo "Builds relative to test/integration/flecs-modules-test/flecs"
cd test/integration/flecs-modules-test/flecs

# ---
echo
echo "Test running test/core"
../../../../build/bake run test/core -- -j 12

# ---
echo
echo "Test running test/core in empty environment"
../../../../build/bake clean
../../../../build/bake reset
../../../../build/bake run test/core -- -j 12

# ---
echo
echo "Listing (should show flecs + modules, not test cases)"
../../../../build/bake list

# ---
cd ../../../..

# ---
echo
echo "Reset environment"
./build/bake clean test
./build/bake reset
echo "Listing (should be empty)"
./build/bake list
echo



# ---
rm -rf ./test/tmp/bake_home

# --
echo
echo "Verify code tree is clean after cleaning"
git status
