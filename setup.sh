#!/bin/sh
make clean
make -j 8 OPTIMIZE=0 # Faster installs allow for quicker container workflows
$(pwd)/build/bake setup
