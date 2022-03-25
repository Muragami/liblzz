#!/bin/bash -x
gcc -Wall -march=native -mtune=native -m64 -s -O3 -S lzz.c
