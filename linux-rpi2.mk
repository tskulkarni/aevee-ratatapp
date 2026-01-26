PLATFORM=linux
ARCH=armv7hf
HAVE_ALSA=1

SYSROOT="/opt/raspberrypi/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian-x64/arm-linux-gnueabihf/libc"
XTOOLS="/opt/raspberrypi/tools/arm-bcm2708/gcc-linaro-arm-linux-gnueabihf-raspbian-x64"

CC=$(XTOOLS)/bin/arm-linux-gnueabihf-gcc
LD=$(XTOOLS)/bin/arm-linux-gnueabihf-gcc
CFLAGS := -DPLATFORM_LINUX -DARCH_ARM -DHAVE_ALSA -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -fPIC -marm -march=armv7-a -mfloat-abi=hard -mfpu=neon -DARM_FPU_VFP_HARD -mthumb-interwork -mtune=cortex-a9 
CFLAGS += --sysroot=$(SYSROOT)
LDFLAGS=

OPTIMIZEFLAGS=-O2 
DEBUGFLAGS=-ggdb3 
