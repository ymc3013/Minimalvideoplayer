# Target name & Directory config
TARGET      := Minimalvideoplayer
BUILD       := build
SOURCES     := source
INCLUDES    := include library/include
LIBDIRS     := library/lib

# Target architecture definition
ARCH        := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

# Compiler / Linker Flags
CFLAGS      := -g -O2 -Wall -ffunction-sections -fdata-sections $(ARCH) -D__3DS__
CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS     := $(ARCH)
LDFLAGS     := -specs=3dsx.specs $(ARCH) -Wl,--gc-sections

# Libraries to link
LIBS        := -lswresample -lavformat -lswscale -lavcodec -lavutil \
               -lx264 -lmp3lame -ldav1d -lz -lm -lctru

# Metadata for 3DS App
APP_TITLE   := Minimal Video Player
APP_DESCRIPTION := Automatic AVI Player
APP_AUTHOR  := ymc3013

# Include DevkitPro build environment rules
include $(DEVKITARM)/base_tools
include $(DEVKITARM)/3ds_rules
