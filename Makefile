#---------------------------------------------------------------------------------
# blox-ps3 Makefile (PSL1GHT). Normally you don't call this directly:
# use linux/build.sh or windows/build.bat.
#   make        -> blox-ps3.elf + blox-ps3.self (+ .fake.self)   (RPCS3)
#   make pkg    -> blox-ps3.pkg only: one package, .self files removed
#                  add KEEP_SELF=1 to keep them
# Packaging uses PSL1GHT's own rules (sprxlinker, make_self_npdrm, ...). Skipping
# sprxlinker produces an app that crashes right at startup (black screen -> XMB).
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "PSL1GHT is not set. Use linux/build.sh or windows/build.bat (Docker) or export PSL1GHT=<path>")
endif

include $(PSL1GHT)/ppu_rules

TARGET    := blox-ps3
BUILD     := build
SOURCES   := source
INCLUDES  :=

TITLE     := Blox PS3 Prototype
APPID     := BLOX00001
CONTENTID := UP0001-$(APPID)_00-0000000000000000

CFLAGS   = -O3 -ffast-math -Wall -std=gnu99 -mcpu=cell -mtune=cell $(MACHDEP) $(INCLUDE)
LDFLAGS  = $(MACHDEP) -Wl,-Map,$(notdir $@).map
LIBS     := -lrsx -lgcm_sys -lio -lsysutil -lrt -llv2 -lm

ifeq ($(SUBBUILD),)

export OUTPUT   := $(CURDIR)/$(TARGET)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export BUILDDIR := $(CURDIR)/$(BUILD)
export DEPSDIR  := $(BUILDDIR)
# absolute paths, used by PSL1GHT's %.pkg rule (which runs inside build/)
export SFOXML   := $(CURDIR)/sfo.xml
export ICON0    := $(CURDIR)/ICON0.PNG

CFILES  := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
export OFILES   := $(CFILES:.c=.o)

# PSL1GHT headers/libs live in $(PSL1GHT)/ppu/{include,lib}. Use what ppu_rules
# provides (LIBPSL1GHT_INC/LIB) and also add any of these dirs that exist.
PSL_INC_DIRS := $(wildcard $(PSL1GHT)/ppu/include $(PS3DEV)/ppu/include $(PORTLIBS)/include)
PSL_LIB_DIRS := $(wildcard $(PSL1GHT)/ppu/lib $(PS3DEV)/ppu/lib $(PORTLIBS)/lib)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(LIBPSL1GHT_INC) $(addprefix -I,$(PSL_INC_DIRS)) -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(LIBPSL1GHT_LIB) $(addprefix -L,$(PSL_LIB_DIRS))

.PHONY: $(BUILD) clean pkg

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile SUBBUILD=1

# Builds exactly ONE installable package: blox-ps3.pkg.
# PSL1GHT's rule can emit both blox-ps3.pkg and blox-ps3.gnpdrm.pkg; we keep the
# gnpdrm one if it exists (the finalized, installable build), rename it to
# blox-ps3.pkg and delete the other, so there is never more than one to pick from.
pkg:
	@[ -d $(BUILD) ] || mkdir -p $(BUILD)
	@rm -f $(OUTPUT).pkg $(OUTPUT).gnpdrm.pkg
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile SUBBUILD=1 $(OUTPUT).pkg
	@if [ -f $(OUTPUT).gnpdrm.pkg ]; then mv -f $(OUTPUT).gnpdrm.pkg $(OUTPUT).pkg; fi
	@if [ -z "$(KEEP_SELF)" ]; then rm -f $(OUTPUT).self $(OUTPUT).fake.self; fi
	@echo "==> Install this file: $(OUTPUT).pkg"

clean:
	@echo clean ...
	@rm -rf $(BUILD) $(OUTPUT).elf $(OUTPUT)*.self $(OUTPUT).a $(OUTPUT)*.pkg

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
# Link through the PPU gcc driver ourselves. The toolchain's default link rule
# calls plain `ld` here, which chokes on gcc-style flags (-mhard-float).
$(OUTPUT).elf: $(OFILES)
	@echo linking ... $(notdir $@)
	@$(CC) $(OFILES) $(LDFLAGS) $(LIBPATHS) $(LIBS) -o $@

-include $(DEPENDS)

endif
