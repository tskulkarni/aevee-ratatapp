SUBDIR=external/jansson

jansson_CFLAGS   := -DHAVE_CONFIG_H
jansson_DEPS     := 
jansson_SOURCES  := dump.c 		\
		    error.c 		\
		    hashtable.c		\
		    hashtable_seed.c	\
		    load.c		\
		    memory.c		\
		    pack_unpack.c	\
		    strbuffer.c		\
		    strconv.c		\
		    utf.c		\
		    value.c		\

$(eval $(call BUILD_STATICLIB,jansson))

