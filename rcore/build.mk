SUBDIR=rcore
#
# rcore build
#

rcore_DEPS     := lua luv uv jansson 
rcore_SOURCES  :=  \
    rc_allocator.c \
    rc_base.c \
    rc_dict.c \
    rc_guid.c \
    rc_list.c \
    rc_status.c \
    rc_string.c \
    rc_netutil.c \
    rc_log.c

$(eval $(call BUILD_STATICLIB,rcore))









