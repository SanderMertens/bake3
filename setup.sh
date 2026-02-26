#!/bin/sh
make clean
make -j 8
$(pwd)/build/bake setup
