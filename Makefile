
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not defined)
endif

TOPDIR := $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET := Minimalvideoplayer
BUILD := build
SOURCES := source
INCLUDES := third_party/decoder/include
LIBDIRS := third_party/decoder/lib

APP_TITLE := Minimal Video Player
APP_DESCRIPTION := Automatic AVI video player
APP_AUTHOR := ymc3013

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS := -O2 -Wall -mword-relocations -ffunction-sections $(ARCH)
CFLAGS += -D__3DS__ -U__STRICT_ANSI__ -std=gnu11

LDFLAGS := -specs=3dsx.specs $(ARCH) -Wl,--gc-sections

LIBS := -lswresample -lavformat -lswscale -lavcodec -lavutil
LIBS += -lcitro2d -lcitro3d -lx264 -lmp3lame -ldav1d
LIBS += -lcurl -lnghttp2 -lmbedtls -lmbedx509 -lmbedcrypto
LIBS += -lctru -lz -lm

.PHONY: all clean

all: 3dsx

clean:
	rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).smdh

