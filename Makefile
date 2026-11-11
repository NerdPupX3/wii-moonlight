#---------------------------------------------------------------------------------
# Clear the implicit built in rules
#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------
# Check that DEVKITPPC is set
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC in your environment. export DEVKITPPC=<path to>devkitPPC")
endif

include $(DEVKITPPC)/wii_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# INCLUDES is a list of directories containing extra header files
#---------------------------------------------------------------------------------
TARGET		:=	boot
BUILD		:=	build
SOURCES     := source \
               source/uuid \
               source/openssl \
               extern/moonlight-common-c/src \
               extern/moonlight-common-c/enet \
               extern/moonlight-common-c/nanors \
               extern/moonlight-common-c/nanors/deps/obl

INCLUDES    := source \
               source/uuid \
               source/openssl \
               extern/moonlight-common-c/src \
               extern/moonlight-common-c/enet/include \
               extern/moonlight-common-c/nanors \
               extern/moonlight-common-c/nanors/deps \
               extern/moonlight-common-c/nanors/deps/obl \
               extern/mbedtls-install/include \
               extern/opus-install/include \
               extern/ffmpeg \
               extern
#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
CFLAGS      = -g -O2 -Wall -Wno-pointer-sign -Wno-unused-parameter -include errno.h -D__WII__ -DHAS_SOCKLEN_T -DNDEBUG -DMBEDTLS_ALLOW_PRIVATE_ACCESS \
              -DMBEDTLS_USER_CONFIG_FILE='"mbedtls_wii_config.h"' $(MACHDEP) $(INCLUDE)

LDFLAGS		=	-g $(MACHDEP)

#---------------------------------------------------------------------------------
# any extra libraries we wish to link with the project
#---------------------------------------------------------------------------------
LIBS        := -lavcodec -lswscale -lavutil \
               -lmbedtls -lmbedx509 -lmbedcrypto -lfat -lwiiuse -lbte -lopus -lasnd -logc -lm -lpthread

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS     := $(PORTLIBS) $(CURDIR)/extern/mbedtls-install

#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(filter-out win32.c rs16.c rs16_afft.c oblas16.c oblas16_afft.c, $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
sFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.S)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(sFILES:.s=.o) $(SFILES:.S=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
					-I$(CURDIR)/$(BUILD) \
					-I$(DEVKITPRO)/libogc/include

export LIBPATHS	:=	-L$(DEVKITPRO)/libogc/lib/wii \
					-L$(CURDIR)/extern/mbedtls-install/lib \
					-L$(CURDIR)/extern/opus-install/lib \
					-L$(CURDIR)/extern/ffmpeg/libavcodec \
					-L$(CURDIR)/extern/ffmpeg/libavutil \
					-L$(CURDIR)/extern/ffmpeg/libswscale

.PHONY: $(BUILD) clean

#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).elf $(TARGET).dol dist

#---------------------------------------------------------------------------------
# Release zip: unzip it onto the SD card root and the app appears in the
# Homebrew Channel (apps/wii-moonlight/boot.dol + meta.xml + icon.png).
#---------------------------------------------------------------------------------
VERSION	:=	$(shell sed -n 's:.*<version>\(.*\)</version>.*:\1:p' hbc/meta.xml)
DISTDIR	:=	dist/stage/apps/wii-moonlight

.PHONY: dist
dist: $(BUILD)
	@rm -rf dist/stage && mkdir -p $(DISTDIR)
	@cp $(TARGET).dol $(DISTDIR)/boot.dol
	@cp hbc/meta.xml hbc/icon.png $(DISTDIR)/
	@rm -f dist/wii-moonlight-$(VERSION).zip
	@cd dist/stage && python3 -m zipfile -c ../wii-moonlight-$(VERSION).zip apps
	@rm -rf dist/stage
	@echo built dist/wii-moonlight-$(VERSION).zip

#---------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
all	:	$(OUTPUT).dol

$(OUTPUT).dol	:	$(OUTPUT).elf

$(OUTPUT).elf	:	$(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
