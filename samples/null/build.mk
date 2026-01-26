SUBDIR=samples/null

#
# null sample build
#
raat_null_sample_DEPS = raat	\
			raat_output_null \
			raat_volume_dummy \
			raat_transport_test

raat_null_sample_SOURCES := raat_null_sample.c 

$(eval $(call BUILD_CONSOLEAPP,raat_null_sample))
