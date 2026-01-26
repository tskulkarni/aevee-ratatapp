SUBDIR=external/luv

luv_DEPS     := lua uv
luv_SOURCES  := luv.c

$(eval $(call BUILD_STATICLIB,luv))

