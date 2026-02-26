#!/bin/sh
make clean
make -j 8 OPTIMIZE=1
$(pwd)/build/bake setup
