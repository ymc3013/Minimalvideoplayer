
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

LDFLAGS := -specs=3dsx.specs $(ARCH) -Wl,--gc-sections

# Bibliothèques principales du décodeur.
DECODER_LIBS := \
	$(DECODER_LIBDIR)/libswresample.a \
	$(DECODER_LIBDIR)/libavformat.a \
	$(DECODER_LIBDIR)/libswscale.a \
	$(DECODER_LIBDIR)/libavcodec.a \
	$(DECODER_LIBDIR)/libavutil.a

# Bibliothèques complémentaires fournies par le projet.
EXTRA_LIBS := \
	$(DECODER_LIBDIR)/libx264.a \
	$(DECODER_LIBDIR)/libmp3lame.a \
	$(DECODER_LIBDIR)/libdav1d.a \
	$(DECODER_LIBDIR)/libcurl.a \
	$(DECODER_LIBDIR)/libnghttp2.a \
	$(DECODER_LIBDIR)/libmbedtls.a \
	$(DECODER_LIBDIR)/libmbedx509.a \
	$(DECODER_LIBDIR)/libmbedcrypto.a

# Bibliothèques du SDK devkitPro.
# Ne pas ajouter -L$(DECODER_LIBDIR) : cela pourrait sélectionner
# une ancienne copie de libctru.a ou libz.a.
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
			echo "ERREUR : bibliothèque manquante : $$lib"; \
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
