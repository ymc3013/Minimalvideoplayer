.SUFFIXES:

ifeq ($(strip $(DEVKITARM)),)
$(error DEVKITARM is not defined)
endif

ifeq ($(strip $(DEVKITPRO)),)
$(error DEVKITPRO is not defined)
endif

TOPDIR := $(CURDIR)
include $(DEVKITARM)/3ds_rules

TARGET := Minimalvideoplayer
BUILD := build
SOURCES := source

INCLUDES := third_party/decoder/include
DECODER_LIBDIR := third_party/decoder/lib

APP_TITLE := Minimal Video Player
APP_DESCRIPTION := Automatic AVI video player
APP_AUTHOR := ymc3013

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS := -O2 -Wall -mword-relocations -ffunction-sections $(ARCH)
CFLAGS += -D__3DS__ -U__STRICT_ANSI__ -std=gnu11
CFLAGS += $(foreach dir,$(INCLUDES),-I$(dir))

# Explicit system library directories from devkitPro
SYSTEM_LIBDIRS := \
	-L$(DEVKITPRO)/libctru/lib \
	-L$(DEVKITPRO)/citro2d/lib \
	-L$(DEVKITPRO)/citro3d/lib \
	-L$(DEVKITPRO)/libz/lib

LDFLAGS := -specs=3dsx.specs $(ARCH) -Wl,--gc-sections

# FFmpeg libraries bundled in the project
DECODER_LIBS := \
	$(DECODER_LIBDIR)/libswresample.a \
	$(DECODER_LIBDIR)/libavformat.a \
	$(DECODER_LIBDIR)/libswscale.a \
	$(DECODER_LIBDIR)/libavcodec.a \
	$(DECODER_LIBDIR)/libavutil.a

# Extra codec / network libs bundled in the project
EXTRA_LIBS := \
	$(DECODER_LIBDIR)/libx264.a \
	$(DECODER_LIBDIR)/libmp3lame.a \
	$(DECODER_LIBDIR)/libdav1d.a \
	$(DECODER_LIBDIR)/libcurl.a \
	$(DECODER_LIBDIR)/libnghttp2.a \
	$(DECODER_LIBDIR)/libmbedtls.a \
	$(DECODER_LIBDIR)/libmbedx509.a \
	$(DECODER_LIBDIR)/libmbedcrypto.a

# System libs from the devkitPro SDK
SYSTEM_LIBS := -lcitro2d -lcitro3d -lctru -lm -lc -lz

CFILES := $(notdir $(wildcard $(SOURCES)/*.c))
OFILES := $(addprefix $(BUILD)/,$(CFILES:.c=.o))

.PHONY: all clean check-libs

all: $(TARGET).3dsx

$(BUILD):
	mkdir -p $(BUILD)

check-libs:
	@set -e; \
	for lib in $(DECODER_LIBS) $(EXTRA_LIBS); do \
		if [ ! -f "$$lib" ]; then \
			echo "ERROR: missing library $$lib"; \
			exit 1; \
		fi; \
	done; \
	for dir in "$(DEVKITPRO)/libctru/lib" "$(DEVKITPRO)/citro2d/lib" "$(DEVKITPRO)/citro3d/lib" "$(DEVKITPRO)/libz/lib"; do \
		if [ ! -d "$$dir" ]; then \
			echo "ERROR: missing directory $$dir"; \
			exit 1; \
		fi; \
	done

$(BUILD)/%.o: $(SOURCES)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TARGET).elf: $(OFILES) | check-libs
	$(CC) $(LDFLAGS) -o $@ $(OFILES) \
		-Wl,--start-group \
		$(SYSTEM_LIBDIRS) \
		$(DECODER_LIBS) \
		$(EXTRA_LIBS) \
		$(SYSTEM_LIBS) \
		-Wl,--end-group

$(TARGET).3dsx: $(TARGET).elf
	$(3DSXTOOL) $< $@

clean:
	rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).smdh

-include $(OFILES:.o=.d)