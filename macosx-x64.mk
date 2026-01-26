PLATFORM=macosx
ARCH=x64

CC=clang
LD=clang
CFLAGS=-DPLATFORM_MACOSX -DARCH_X64 -Wno-deprecated-declarations -msse -march=native
LDFLAGS=

OPTIMIZEFLAGS=-O3 -ffast-math 
DEBUGFLAGS=-ggdb3 
