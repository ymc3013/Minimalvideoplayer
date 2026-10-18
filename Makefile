TARGET      := Minimalvideoplayer
BUILD       := build
SOURCES     := source
INCLUDES    := include

ARCH        := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS      := -g -Wall -O2 -mword-relocations -ffunction-sections $(ARCH)
CFLAGS      += -D__3DS__ -I$(DEVKITPRO)/portlibs/3ds/include
CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions
LDFLAGS     := -specs=3dsx.specs -g $(ARCH) -Wl,--gc-sections

LIBS        := -lctru -lavformat -lavcodec -lswresample -lswscale -lavutil -lm -lz
LIBDIRS     := $(DEVKITPRO)/portlibs/3ds/lib

APP_TITLE       := Minimal Video Player
APP_DESCRIPTION := Automatic AVI video player
APP_AUTHOR      := ymc3013

include $(DEVKITPRO)/libctru/default_icon.png
include $(DEVKITARM)/3ds_rules