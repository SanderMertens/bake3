#!/bin/sh
make clean
make -j 8 OPTIMIZE=0 # Faster installs allow for quicker container workflows
if [ "$(uname)" = "Darwin" ]; then codesign -f -s - "$(pwd)/build/bake"; fi
$(pwd)/build/bake setup
