
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

DECODER_DIR := third_party/decoder
DECODER_LIBDIR := $(DECODER_DIR)/lib

INCLUDES := $(DECODER_DIR)/include
CFLAGS += $(foreach dir,$(INCLUDES),-I$(dir))

APP_TITLE := Minimal Video Player
APP_DESCRIPTION := Automatic AVI video player
APP_AUTHOR := ymc3013

ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS += -O2 -Wall -mword-relocations -ffunction-sections
CFLAGS += $(ARCH) -D__3DS__ -U__STRICT_ANSI__ -std=gnu11

LDFLAGS += -specs=3dsx.specs $(ARCH) -Wl,--gc-sections

# Official SDK library directories.
# The workflow verifies that these libraries actually exist.
LIBDIRS := \
	$(DEVKITPRO)/libctru \
	$(DEVKITPRO)/citro2d \
	$(DEVKITPRO)/citro3d \
	$(DEVKITPRO)/portlibs/3ds

DECODER_LIBS := \
	$(DECODER_LIBDIR)/libswresample.a \
	$(DECODER_LIBDIR)/libavformat.a \
	$(DECODER_LIBDIR)/libswscale.a \
	$(DECODER_LIBDIR)/libavcodec.a \
	$(DECODER_LIBDIR)/libavutil.a

EXTRA_LIBS := \
	$(DECODER_LIBDIR)/libx264.a \
	$(DECODER_LIBDIR)/libmp3lame.a \
	$(DECODER_LIBDIR)/libdav1d.a \
	$(DECODER_LIBDIR)/libcurl.a \
	$(DECODER_LIBDIR)/libnghttp2.a \
	$(DECODER_LIBDIR)/libmbedtls.a \
	$(DECODER_LIBDIR)/libmbedx509.a \
	$(DECODER_LIBDIR)/libmbedcrypto.a

SYSTEM_LIBS := -lcitro2d -lcitro3d -lctru -lz -lm -lc

CFILES := $(notdir $(wildcard $(SOURCES)/*.c))
OFILES := $(addprefix $(BUILD)/,$(CFILES:.c=.o))

.PHONY: all clean check-libs

all: $(TARGET).3dsx

$(BUILD):
	mkdir -p $@

check-libs:
	@set -eu; \
	for lib in $(DECODER_LIBS) $(EXTRA_LIBS); do \
		if [ ! -s "$$lib" ]; then \
			echo "ERROR: Missing or empty archive: $$lib"; \
			exit 1; \
		fi; \
	done
	@set -eu; \
	for lib in libctru.a libcitro2d.a libcitro3d.a libz.a; do \
		found=0; \
		for dir in $(LIBDIRS); do \
			if [ -s "$$dir/lib/$$lib" ]; then \
				found=1; \
				break; \
			fi; \
		done; \
		if [ "$$found" -eq 0 ]; then \
			echo "ERROR: SDK archive not found: $$lib"; \
			echo "Check the installed devkitPro packages and LIBDIRS."; \
			exit 1; \
		fi; \
	done

$(BUILD)/%.o: $(SOURCES)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(TARGET).elf: $(OFILES) | check-libs
	$(CC) $(LDFLAGS) -o $@ $(OFILES) \
		-Wl,--start-group \
		$(DECODER_LIBS) \
		$(EXTRA_LIBS) \
		$(SYSTEM_LIBS) \
		-Wl,--end-group

$(TARGET).3dsx: $(TARGET).elf
	$(3DSXTOOL) $< $@

clean:
	rm -rf $(BUILD) $(TARGET).3dsx $(TARGET).elf $(TARGET).smdh

-include $(OFILES:.o=.d)
