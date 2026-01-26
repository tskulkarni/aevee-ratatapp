PLATFORM=macosx
ARCH=x86

CC=clang
LD=clang
CFLAGS=-m32 -DPLATFORM_MACOSX -DARCH_X86 -Wno-deprecated-declarations
LDFLAGS=-m32

OPTIMIZEFLAGS=-O2 -fomit-frame-pointer -ffast-math 
DEBUGFLAGS=-ggdb3 
