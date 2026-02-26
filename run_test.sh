#/bin/sh

# Build bake
make clean
make -j 8

export BAKE_HOME="./test/tmp/bake_home"

## ---
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

## ---
echo
echo "Reset environment"
./build/bake clean test
./build/bake reset
echo "Listing (should be empty)"
./build/bake list
echo

## ---
echo
echo "Build test/projects"
./build/bake build test/projects
echo "Listing (should be empty)"
./build/bake list
echo

## ---
echo
echo "Reset environment"
./build/bake clean test
./build/bake reset
echo "Listing (should be empty)"
./build/bake list
echo

## ---
echo
echo "Build test/integration"
./build/bake build test/integration
echo "Listing (should show flecs + modules)"
./build/bake list
echo

## ---
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

## ---
echo
echo "Reset environment"
./build/bake clean test
./build/bake reset
echo "Listing (should be empty)"
./build/bake list
echo
