SUBDIR=external/uv

uv_CFLAGS   := 
uv_LDFLAGS := 
uv_SOURCES  := \
        fs-poll.c \
        inet.c \
        threadpool.c \
        uv-common.c \
        version.c \
	unix/async.c \
	unix/core.c \
	unix/dl.c \
	unix/fs.c \
	unix/getaddrinfo.c \
	unix/getnameinfo.c \
	unix/loop.c \
	unix/loop-watcher.c \
	unix/pipe.c \
	unix/poll.c \
	unix/process.c \
	unix/signal.c \
	unix/stream.c \
	unix/tcp.c \
	unix/thread.c \
	unix/timer.c \
	unix/tty.c \
	unix/udp.c

ifeq ($(PLATFORM),linux) 
    uv_SOURCES += \
	unix/linux-core.c \
	unix/linux-inotify.c \
	unix/linux-syscalls.c \
	unix/proctitle.c 
    uv_CFLAGS += -D_GNU_SOURCE
    uv_LDFLAGS += -lm -lpthread -lrt -ldl
endif

ifeq ($(PLATFORM),macosx) 
    uv_SOURCES += \
	unix/darwin-proctitle.c \
	unix/proctitle.c \
	unix/darwin.c \
	unix/fsevents.c \
	unix/kqueue.c 
    uv_CFLAGS= -D_DARWIN_USE_64_BIT_INODE -D_DARWIN_UNLIMITED_SELECT
    uv_LDFLAGS=-lm -lpthread
endif

$(eval $(call BUILD_STATICLIB,uv))

