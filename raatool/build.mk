SUBDIR=raatool

#
# raatool build
#
raatool_DEPS    = raat

raatool_SOURCES := raatool_main.c 

$(eval $(call BUILD_CONSOLEAPP,raatool))
