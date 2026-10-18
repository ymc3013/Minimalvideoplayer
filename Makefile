TARGET := Minimalvideoplayer
BUILD := build
SOURCES := source
INCLUDES := include library/include
LIBDIRS := library/lib

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS := -g -O2 -Wall -ffunction-sections -fdata-sections $(ARCH)
CFLAGS += -D__3DS__
CXXFLAGS := $(CFLAGS) -fno-rtti -fno-exceptions
ASFLAGS := $(ARCH)
LDFLAGS := -specs=3dsx.specs $(ARCH) -Wl,--gc-sections

LIBS := -lswresample -lavformat -lswscale -lavcodec -lavutil
LIBS += -lx264 -lmp3lame -ldav1d -lz -lm -lctru

APP_TITLE := Minimal Video Player
APP_DESCRIPTION := Automatic AVI Player
APP_AUTHOR := ymc3013

include $(DEVKITARM)/3ds_rules