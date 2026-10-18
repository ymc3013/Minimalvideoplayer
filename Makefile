TARGET := Minimalvideoplayer
BUILD := build
SOURCES := source
INCLUDES := include

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS := -g -O2 -Wall -mword-relocations -ffunction-sections
CFLAGS += $(ARCH) -D__3DS__

CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS := $(ARCH)
LDFLAGS := -specs=3dsx.specs $(ARCH) -Wl,--gc-sections

LIBS := -lctru

APP_TITLE := Minimalvideoplayer
APP_DESCRIPTION := Minimal video player
APP_AUTHOR := ymc3013

include $(DEVKITPRO)/devkitARM/3ds_rules