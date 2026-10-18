
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

SYSTEM_LIBDIRS := \
	$(SDK_CTRU_DIR) \
	$(SDK_CITRO2D_DIR) \
	$(SDK_CITRO3D_DIR) \
	$(SDK_ZLIB_DIR)

LDFLAGS += -specs=3dsx.specs $(ARCH) -Wl,--gc-sections
LDFLAGS += $(foreach dir,$(SYSTEM_LIBDIRS),-L$(dir))

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
		if [ ! -s "$$lib" ]; then \
			echo "ERROR: missing or empty library $$lib"; \
			exit 1; \
		fi; \
	done; \
	for lib in \
		"$(SDK_CTRU_DIR)/libctru.a" \
		"$(SDK_CITRO2D_DIR)/libcitro2d.a" \
		"$(SDK_CITRO3D_DIR)/libcitro3d.a" \
		"$(SDK_ZLIB_DIR)/libz.a"; do \
		if [ ! -s "$$lib" ]; then \
			echo "ERROR: SDK library not found: $$lib"; \
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
