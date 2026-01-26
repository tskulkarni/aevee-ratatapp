TOP:=$(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))

CONFIG?=debug

TARGET?=default-target

ifeq ("$(wildcard $(TARGET).mk)","")
$(info )
$(info This Makefile supports building for multiple different target platforms in the same tree.)
$(info )
$(info As a result, you must tell the Makefile which target you are building for. Configurations)
$(info for some common targets are located in this directory. See macosx-x64.mk, linux-x64.mk, and )
$(info linux-rpi2.mk for some examples.)
$(info )
$(info If you are building for one of these standard platforms, you may be able to use one of the default target files. If )
$(info you are building for a custom platform, or using your own toolchain, it is likely that you will need to define your own.)
$(info )
$(info There are two ways to set the target platform. One is to explicitly invoke make with a TARGET= variable like this:)
$(info )
$(info $$ make TARGET=macosx-x86)
$(info )
$(info The other way is to symlink your target to "default-target.mk" in this directory. This will be more convnnient if you only build for one target:)
$(info )
$(info $$ ln -s macosx-x86.mk default-target.mk)
$(info )
$(error )
endif

include $(TARGET).mk

OBJDIR=obj/$(CONFIG)/$(PLATFORM)/$(ARCH)/
BINDIR=bin/$(CONFIG)/$(PLATFORM)/$(ARCH)/

ifneq ($(PLATFORM),windows)
CFLAGS += -MMD
endif

ifeq ($(CONFIG),release)
CFLAGS += $(OPTIMIZEFLAGS)
endif

ifeq ($(CONFIG),debug)
CFLAGS += $(DEBUGFLAGS)
endif

all: 

define uniq
$(eval seen :=)
$(foreach _,$1,$(if $(filter $_,${seen}),,$(eval seen += $_)))
${seen}
endef

define BUILD_STATICLIB
$(1)_TARGET   := $(BINDIR)/lib$(1).a
$(1): $$($(1)_TARGET)
$(1)_OBJECTS  := $(foreach src,$($(1)_SOURCES),$(OBJDIR)/$(1)/$(src:.c=.o))
$(1)_INCLUDE   = -I$(SUBDIR) $(foreach dep,$($(1)_DEPS),$$($(dep)_INCLUDE))
$(1)_LINKDEPS += $$($(1)_TARGET) $(foreach dep,$($(1)_DEPS),$$($(dep)_LINKDEPS))
$(1)_LDFLAGS  += $$($(1)_TARGET) $(foreach dep,$($(1)_DEPS),$$($(dep)_LDFLAGS))
$(1)_LINKDEPS := $$(strip $$(call uniq,$$($(1)_LINKDEPS)))
$(1)_LDFLAGS  := $$(strip $$(call uniq,$$($(1)_LDFLAGS)))
$(foreach source,$($(1)_SOURCES),
-include $(OBJDIR)/$(1)/$(source:.c=.d)
$(OBJDIR)/$(1)/$(source:.c=.o): $(SUBDIR)/$(source)
	@mkdir -p $$(dir $$@)
	$$(CC) -o $$@ -c $$(CFLAGS) $$($(1)_CFLAGS) $$($(1)_INCLUDE) $(SUBDIR)/$(source)
)
$$($(1)_TARGET): $$($(1)_OBJECTS) 
	@mkdir -p $$(dir $$@)
	$$(AR) rcs $$@ $$^
ALL_TARGETS += $$($(1)_TARGET)
endef

define BUILD_CONSOLEAPP
$(1)_TARGET   := $(BINDIR)/$(1)
$(1): $$($(1)_TARGET)
$(1)_OBJECTS  := $(foreach src,$($(1)_SOURCES),$(OBJDIR)/$(1)/$(src:.c=.o))
$(1)_INCLUDE   = -I$(SUBDIR) $(foreach dep,$($(1)_DEPS),$$($(dep)_INCLUDE))
$(1)_LDFLAGS  += $(foreach dep,$($(1)_DEPS),$$($(dep)_LDFLAGS))
$(1)_LINKDEPS += $(foreach dep,$($(1)_DEPS),$$($(dep)_LINKDEPS))
$(1)_LINKDEPS := $$(strip $$(call uniq,$$($(1)_LINKDEPS)))
$(1)_LDFLAGS  := $$(strip $$(call uniq,$$($(1)_LDFLAGS)))
$(foreach source,$($(1)_SOURCES),
-include $(OBJDIR)/$(1)/$(source:.c=.d)
$(OBJDIR)/$(1)/$(source:.c=.o): $(SUBDIR)/$(source) 
	@mkdir -p $$(dir $$@)
	$$(CC) -o $$@ -c $$(CFLAGS) $$($(1)_CFLAGS) $$($(1)_INCLUDE) $(SUBDIR)/$(source)
)
$$($(1)_TARGET): $$($(1)_OBJECTS) $$($(1)_LINKDEPS)
	@mkdir -p $$(dir $$@)
	$$(LD) -o $$@ $$^ $$($(1)_LDFLAGS) $$(LDFLAGS) 
ALL_TARGETS += $$($(1)_TARGET)
endef

define BUILD_SHAREDLIB
$(1)_TARGET   := $(BINDIR)/lib$(1).so
$(1): $$($(1)_TARGET)
$(1)_OBJECTS  := $(foreach src,$($(1)_SOURCES),$(OBJDIR)/$(1)/$(src:.c=.o))
$(1)_INCLUDE   = -I$(SUBDIR) $(foreach dep,$($(1)_DEPS),$$($(dep)_INCLUDE))
$(1)_LDFLAGS  += $(foreach dep,$($(1)_DEPS),$$($(dep)_LDFLAGS))
$(1)_LINKDEPS += $(foreach dep,$($(1)_DEPS),$$($(dep)_LINKDEPS))
$(1)_LINKDEPS := $$(strip $$(call uniq,$$($(1)_LINKDEPS)))
$(1)_LDFLAGS  := $$(strip $$(call uniq,$$($(1)_LDFLAGS)))
$(foreach source,$($(1)_SOURCES),
-include $(OBJDIR)/$(1)/$(source:.c=.d)
$(OBJDIR)/$(1)/$(source:.c=.o): $(SUBDIR)/$(source) 
	@mkdir -p $$(dir $$@)
	$$(CC) -o $$@ -c $$(CFLAGS) $$($(1)_CFLAGS) $$($(1)_INCLUDE) $(SUBDIR)/$(source)
)
$$($(1)_TARGET): $$($(1)_OBJECTS) $$($(1)_LINKDEPS)
	@mkdir -p $$(dir $$@)
	$$(CC) -shared -o $$@ $$^ $$($(1)_LDFLAGS) $$(LDFLAGS) 
ALL_TARGETS += $$($(1)_TARGET)
endef

ALL_TARGETS :=

include external/build.mk
include rcore/build.mk
include raat/build.mk
include plugins/build.mk
include samples/build.mk
include raat_app/build.mk
include raatool/build.mk
-include custom/build.mk

all: $(ALL_TARGETS)

docs: FORCE
	doxygen
FORCE:

cleandocs:
	rm -Rf docs

distclean: 
	rm -Rf bin obj docs

clean:
	rm -Rf $(BINDIR) $(OBJDIR)

.PHONY: clean all
