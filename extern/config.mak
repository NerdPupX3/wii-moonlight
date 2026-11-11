# Nintendo Wii (Broadway / PPC 750CL) cross-compile config for FFmpeg's H.264 decoder.
# Hand-written to avoid running the old (fragile) FFmpeg configure on a bare-metal target.
DEVKITPPC ?= /opt/devkitpro/devkitPPC

AR      = $(DEVKITPPC)/bin/powerpc-eabi-ar
AS      = $(DEVKITPPC)/bin/powerpc-eabi-as
CC      = $(DEVKITPPC)/bin/powerpc-eabi-gcc
LD      = $(DEVKITPPC)/bin/powerpc-eabi-ld
RANLIB  = $(DEVKITPPC)/bin/powerpc-eabi-ranlib
STRIP   = $(DEVKITPPC)/bin/powerpc-eabi-strip

# Broadway / Wii flags (same as MPlayer CE / WiiMC)
CFLAGS = -O3 -mcpu=750 -meabi -mrvl -msdata -mmultiple -pipe \
         -ffast-math -frename-registers \
         -DGEKKO -DHW_RVL -D__WII__ \
         -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE \
         -std=gnu99 \
         -Wno-incompatible-pointer-types -Wno-int-conversion \
         -Wno-implicit-function-declaration -Wno-pointer-sign \
         -Wno-switch -Wno-parentheses -Wno-unused-result \
         -I.

ASFLAGS = $(CFLAGS)
CPPFLAGS =
CXXFLAGS =
LDFLAGS =

ARCH     = ppc
ARCH_PPC = yes
GEKKO    = yes

# Feature toggles the makefiles read (must mirror config.h)
HAVE_PAIRED  = yes
HAVE_ALTIVEC = no

# In-tree build
SRC_PATH = .
CONFIG_FAKE_MONO = yes
HAVE_INTTYPES_H = yes
HAVE_ERRNO_H = yes
HAVE_LIMITS_H = yes
HAVE_UNISTD_H = yes
HAVE_MEMCPY = yes
HAVE_DLFCN_H = yes
HAVE_DVDCSS_DVDCSS_H = yes
HAVE_ALLOCA_H = yes
HAVE_MALLOC_H = yes
HAVE_GETTIMEOFDAY = yes
HAVE_MEMALIGN = yes
HAVE_AUDIO_SELECT = yes
HAVE_SETENV = yes
HAVE_STRSEP = yes
HAVE_VSSCANF = yes
HAVE_BUILTIN_EXPECT = yes
CONFIG_ICONV = yes
CONFIG_FASTMEMCPY = yes
CONFIG_SORTSUB = yes
CONFIG_STREAM_CACHE = yes
HAVE_BIGENDIAN = yes
ARCH_PPC = yes
HAVE_PAIRED = yes
HAVE_FAST_CLZ = yes
CONFIG_DVDNAV = yes
CONFIG_DVDREAD = yes
CONFIG_ZLIB = yes
CONFIG_ASS = yes
CONFIG_ASS_INTERNAL = yes
CONFIG_FREETYPE = yes
CONFIG_FTP = yes
HAVE_INET_ATON = yes
CONFIG_NETWORKING = yes
HAVE_SOCKLEN_T = yes
CONFIG_FFMPEG = yes
CONFIG_FFMPEG_A = yes
CONFIG_DECODERS = yes
CONFIG_DEMUXERS = yes
HAVE_CBRTF = yes
HAVE_EXP2 = yes
HAVE_EXP2F = yes
HAVE_FAST_UNALIGNED = yes
HAVE_LLRINT = yes
HAVE_LLRINTF = yes
HAVE_LOCAL_ALIGNED_8 = yes
HAVE_LOCAL_ALIGNED_16 = yes
HAVE_LOG2 = yes
HAVE_LOG2F = yes
HAVE_LRINT = yes
HAVE_LRINTF = yes
HAVE_MKSTEMP = yes
CONFIG_NETWORK = yes
HAVE_ROUND = yes
HAVE_ROUNDF = yes
HAVE_TRUNC = yes
HAVE_TRUNCF = yes
CONFIG_FASTDIV = yes
CONFIG_GPL = yes
CONFIG_SWSCALE_ALPHA = yes
HAVE_ATTRIBUTE_PACKED = yes
CONFIG_GOLOMB = yes
CONFIG_H264DSP = yes
CONFIG_H264PRED = yes
CONFIG_H264_DECODER = yes
CONFIG_H264_PARSER = yes
CONFIG_AVCODEC = yes
CONFIG_SWSCALE = yes

# ---- FFmpeg library build plumbing (from the configure-generated config.mak) ----
CONFIG_STATIC = yes
CONFIG_SHARED = no
LIBPREF   = lib
LIBSUF    = .a
BUILDSUF  =
FULLNAME  = $(NAME)$(BUILDSUF)
LIBNAME   = $(LIBPREF)$(FULLNAME)$(LIBSUF)
SLIBPREF  = lib
SLIBSUF   = .so
SLIBNAME  = $(SLIBPREF)$(FULLNAME)$(SLIBSUF)
SLIBNAME_WITH_MAJOR = $(SLIBNAME)-42
EXESUF    = .elf
HOSTCC    = gcc
HOSTLD    = gcc
HOSTCFLAGS = -O3 -I.
HOSTEXESUF =

# Compiler output flag (missing CC_O caused .o files to land in cwd, not SUBDIR/)
CC_O = -o $@
AS_O = -o $@
CC_DEPFLAGS = -MD -MP
AS_DEPFLAGS = -MD -MP
