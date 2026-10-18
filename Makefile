
.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM n'est pas défini)
endif

TOPDIR := $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET := Minimalvideoplayer
BUILD := build
SOURCES := source
INCLUDES := third_party/decoder/include
LIBDIRS := third_party/decoder/lib

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS := -O2 -Wall -ffunction-sections $(ARCH) -D__3DS__
CFLAGS += -I$(TOPDIR)/third_party/decoder/include

LDFLAGS := -specs=3dsx.specs $(ARCH) -Wl,--gc-sections

LIBS := \
    -L$(TOPDIR)/third_party/decoder/lib \
    -lavformat \
    -lavcodec \
    -lswresample \
    -lswscale \
    $(TOPDIR)/third_party/decoder/lib/lubavutil.a \
    -lctru -lm

.PHONY: all clean

all:
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SOURCES)/main.c \
	    $(LDFLAGS) $(LIBS) \
	    -o $(BUILD)/$(TARGET).elf
	3dsxtool $(BUILD)/$(TARGET).elf $(TARGET).3dsx

clean:
	rm -rf $(BUILD) $(TARGET).3dsx
