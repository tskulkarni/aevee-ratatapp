SUBDIR=raat
#
# raat build
#

raat_DEPS     := lua luv uv jansson rcore
raat_SOURCES  :=  \
    raat_base.c \
    raat_client.c \
    raat_device.c \
    raat_discovery.c \
    raat_dsp.c \
    raat_info.c \
    raat_log.c \
    raat_plugin_output.c \
    raat_plugin_source_selection.c \
    raat_plugin_transport.c \
    raat_plugin_volume.c \
    raat_script.c \
    raat_script_buffer.c \
    raat_script_info.c \
    raat_script_log.c \
    raat_script_plugin_output.c \
    raat_script_plugin_source_selection.c \
    raat_script_plugin_transport.c \
    raat_script_plugin_volume.c \
    raat_script_session.c \
    raat_script_stream.c \
    raat_server.c \
    raat_session.c \
    raat_stream.c

$(eval $(call BUILD_STATICLIB,raat))

