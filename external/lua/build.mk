SUBDIR=external/lua

lua_CFLAGS   := 
lua_LDFLAGS   := 
lua_SOURCES  := \
        lapi.c \
        lauxlib.c \
        lbaselib.c \
        lbitlib.c \
        lcode.c \
        lcorolib.c \
        lctype.c \
        ldblib.c \
        ldebug.c \
        ldo.c \
        ldump.c \
        lfunc.c \
        lgc.c \
        linit.c \
        liolib.c \
        llex.c \
        lmathlib.c \
        lmem.c \
        loadlib.c \
        lobject.c \
        lopcodes.c \
        loslib.c \
        lparser.c \
        lstate.c \
        lstring.c \
        lstrlib.c \
        ltable.c \
        ltablib.c \
        ltm.c \
        lundump.c \
        lutf8lib.c \
        lvm.c \
        lzio.c 

ifeq ($(PLATFORM),linux)
    lua_CFLAGS += -DLUA_USE_LINUX
    lua_LDFLAGS += -ldl -lpthread -lm
endif

ifeq ($(PLATFORM),macosx)
    lua_CFLAGS += -DLUA_USE_MACOSX
endif

$(eval $(call BUILD_STATICLIB,lua))

#
# luai build
#
luai_DEPS    = lua
luai_SOURCES := lua.c

$(eval $(call BUILD_CONSOLEAPP,luai))

#
# luac build
#
luac_DEPS    = lua
luac_SOURCES := luac.c

$(eval $(call BUILD_CONSOLEAPP,luac))

