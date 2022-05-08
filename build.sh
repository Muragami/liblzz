#!/bin/bash -x
rm *.a
rm *.o
gcc -Wall -march=native -mtune=native -m64 -s -O3 -c lzz.c -o lzz.o
ar rcs liblzz.a lzz.o
gcc -Wall -march=native -mtune=native -m64 -s -O3 plzz.c lzz.o -o plzz.exe ./lib/liblz4.gcc-win64.a