#!/bin/bash -x
rm *.a
rm *.o
gcc -Wall -march=native -mtune=native -m64 -s -O3 -c lzz.c -o lzz.o
ar rcs liblzz.a lzz.o