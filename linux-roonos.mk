PLATFORM=linux
ARCH=x64
HAVE_ALSA=1

CC=x86_64-roon-linux-gnu-gcc
LD=x86_64-roon-linux-gnu-gcc
CFLAGS=-DPLATFORM_LINUX -I../build/alsalib/usr/include -DARCH_X64 -DHAVE_ALSA -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE --sysroot=$(ROON_XTOOLS_SYSROOT)
LDFLAGS=-L../build/alsalib/usr/lib

OPTIMIZEFLAGS=-O2 -fomit-frame-pointer -ffast-math
DEBUGFLAGS=-ggdb3
