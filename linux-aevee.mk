PLATFORM=linux
ARCH=armv7hf
HAVE_ALSA=1

CC=gcc
LD=gcc

CFLAGS := -march=armv7-a -mtune=cortex-a9 -mfpu=neon -mfloat-abi=hard -DHAVE_ALSA -DPLATFORM_LINUX
OPTIMIZEFLAGS=-O2
DEBUGFLAGS=-ggdb3
