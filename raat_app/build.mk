SUBDIR=raat_app

#
# raat_app build
#
raat_app_DEPS    = raat	\
		   raat_transport_test \
		   raat_output_null \
		   raat_output_capture \
		   raat_source_selection_test \
		   raat_volume_dummy \
		   raat_volume_incremental \
		   raat_volume_software \
		   raat_volume_null

ifeq ($(HAVE_ALSA),1)
    raat_app_DEPS += raat_output_alsa 
    raat_app_DEPS += raat_volume_alsa 
    raat_app_DEPS += raat_watch_alsa 
endif

ifeq ($(PLATFORM),macosx)
    raat_app_DEPS += raat_output_coreaudio 
    raat_app_DEPS += raat_volume_coreaudio 
endif

raat_app_SOURCES := raat_app.c 

$(eval $(call BUILD_CONSOLEAPP,raat_app))
