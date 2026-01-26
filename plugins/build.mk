SUBDIR=plugins

#
# transport_test plugin
#
raat_transport_test_DEPS     := raat
raat_transport_test_SOURCES  := raat_plugin_transport_test.c

$(eval $(call BUILD_STATICLIB,raat_transport_test))

#
# output_null plugin
#
raat_output_null_DEPS     := raat
raat_output_null_SOURCES  := raat_plugin_output_null.c

$(eval $(call BUILD_STATICLIB,raat_output_null))

#
# output_capture plugin
#
raat_output_capture_DEPS     := raat
raat_output_capture_SOURCES  := raat_plugin_output_capture.c

$(eval $(call BUILD_STATICLIB,raat_output_capture))

#
# source_selection_test plugin
#
raat_source_selection_test_DEPS     := raat
raat_source_selection_test_SOURCES  := raat_plugin_source_selection_test.c

$(eval $(call BUILD_STATICLIB,raat_source_selection_test))

#
# volume_dummy plugin
#
raat_volume_dummy_DEPS     := raat
raat_volume_dummy_SOURCES  := raat_plugin_volume_dummy.c

$(eval $(call BUILD_STATICLIB,raat_volume_dummy))

#
# volume_incremental plugin (sample:
#
raat_volume_incremental_DEPS     := raat
raat_volume_incremental_SOURCES  := raat_plugin_volume_incremental.c

$(eval $(call BUILD_STATICLIB,raat_volume_incremental))

#
# volume_software plugin
#
raat_volume_software_DEPS     := raat
raat_volume_software_SOURCES  := raat_plugin_volume_software.c

$(eval $(call BUILD_STATICLIB,raat_volume_software))

#
# volume_null plugin
#
raat_volume_null_DEPS     := raat
raat_volume_null_SOURCES  := raat_plugin_volume_null.c

$(eval $(call BUILD_STATICLIB,raat_volume_null))

ifeq ($(HAVE_ALSA), 1)
    #
    # volume_alsa plugin
    #
    raat_volume_alsa_DEPS     := raat
    raat_volume_alsa_SOURCES  := raat_plugin_volume_alsa.c
    raat_volume_alsa_LDFLAGS  := -lasound

    $(eval $(call BUILD_STATICLIB,raat_volume_alsa))

    #
    # output_alsa plugin
    #
    raat_output_alsa_DEPS     := raat
    raat_output_alsa_SOURCES  := raat_plugin_output_alsa.c
    raat_output_alsa_LDFLAGS  := -lasound

    $(eval $(call BUILD_STATICLIB,raat_output_alsa))

    #
    # watch_alsa plugin
    #
    raat_watch_alsa_DEPS     := raat
    raat_watch_alsa_SOURCES  := raat_plugin_watch_alsa.c
    raat_watch_alsa_LDFLAGS  := -lasound

    $(eval $(call BUILD_STATICLIB,raat_watch_alsa))
endif


ifeq ($(PLATFORM), macosx)
    #
    # volume_coreaudio plugin
    #
    raat_volume_coreaudio_DEPS     := raat
    raat_volume_coreaudio_SOURCES  := raat_plugin_volume_coreaudio.c
    raat_volume_coreaudio_LDFLAGS  := \
	/System/Library/Frameworks/AudioUnit.framework/Versions/Current/AudioUnit \
	/System/Library/Frameworks/CoreServices.framework/Versions/Current/CoreServices \
	/System/Library/Frameworks/CoreAudio.framework/Versions/Current/CoreAudio \
	/System/Library/Frameworks/AudioToolbox.framework/Versions/Current/AudioToolbox \
	/System/Library/Frameworks/IOKit.framework/Versions/Current/IOKit \

    $(eval $(call BUILD_STATICLIB,raat_volume_coreaudio))

    #
    # output_coreaudio plugin
    #
    raat_output_coreaudio_DEPS     := raat
    raat_output_coreaudio_SOURCES  := raat_plugin_output_coreaudio.c
    raat_output_coreaudio_LDFLAGS  := \
	/System/Library/Frameworks/AudioUnit.framework/Versions/Current/AudioUnit \
	/System/Library/Frameworks/CoreServices.framework/Versions/Current/CoreServices \
	/System/Library/Frameworks/CoreAudio.framework/Versions/Current/CoreAudio \
	/System/Library/Frameworks/AudioToolbox.framework/Versions/Current/AudioToolbox \
	/System/Library/Frameworks/IOKit.framework/Versions/Current/IOKit \

    $(eval $(call BUILD_STATICLIB,raat_output_coreaudio))
endif


