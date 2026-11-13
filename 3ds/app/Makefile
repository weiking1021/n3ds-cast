.DEFAULT_GOAL := all
include $(DEVKITARM)/3ds_rules
BUILD := build
ARCH := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft
CFLAGS := -O3 -Wall -Wextra -Werror -Wno-unused-parameter -ffunction-sections -fdata-sections $(ARCH) -D__3DS__ -I. -I../shared -I$(CTRULIB)/include -I$(PORTLIBS)/include
# Cast receiver application ID baked into the package: make APP_ID=XXXXXXXX (default: see protocol.h)
# Version shown in the app and sent to the receiver: make APP_VERSION=1.2.3 (default: 0.0.0, see version.h)
ifdef APP_VERSION
CFLAGS += -DNATIVE_APP_VERSION='"$(APP_VERSION)"'
endif
ifdef APP_ID
CFLAGS += -DNATIVE_APP_ID='"$(APP_ID)"'
endif
LDFLAGS := -specs=3dsx.specs $(ARCH) -Wl,--gc-sections
LIBPATH := -L$(CTRULIB)/lib -L$(PORTLIBS)/lib
LIBS := -lmbedtls -lmbedx509 -lmbedcrypto -lctru -lm
SERVICE := hw service service_state service_control service_audio service_overlay service_encoder service_stream capture frame video rects overlay overlay_pixels game_audio audio_stream cast_client cast_proto capture_jpeg
OBJS := $(addprefix $(BUILD)/,$(addsuffix .o,$(SERVICE))) $(BUILD)/luma.o
.PHONY: all
all: n3ds-cast.3dsx
$(BUILD):
	mkdir -p $@
$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/%.o: ../shared/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/luma.o: luma.s | $(BUILD)
	$(CC) $(ARCH) -c $< -o $@
$(BUILD)/resident.elf: $(OBJS)
	$(CC) $(LDFLAGS) $^ $(LIBPATH) $(LIBS) -o $@
$(BUILD)/resident_image.h: $(BUILD)/resident.elf make_image.py
	python3 make_image.py $< $@
$(BUILD)/client.o: client.c $(BUILD)/resident_image.h
	$(CC) $(CFLAGS) -I$(BUILD) -MMD -MP -c $< -o $@
$(BUILD)/frontend.o: frontend.c $(BUILD)/resident_image.h
	$(CC) $(CFLAGS) -I$(BUILD) -MMD -MP -c $< -o $@
$(BUILD)/config.o: ../shared/config.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@
$(BUILD)/font_data.h: assets/ui-font.bcfnt embed_font.py | $(BUILD)
	python3 embed_font.py $< $@
$(BUILD)/ui.o: ui.c $(BUILD)/font_data.h
	$(CC) $(CFLAGS) -I$(BUILD) -MMD -MP -c $< -o $@
n3ds-cast.elf: $(BUILD)/frontend.o $(BUILD)/client.o $(BUILD)/luma.o $(BUILD)/config.o $(BUILD)/discovery.o $(BUILD)/settings.o $(BUILD)/i18n.o $(BUILD)/ui.o
	$(CC) $(LDFLAGS) $^ -L$(CTRULIB)/lib -lcitro2d -lcitro3d -lctru -lm -o $@
n3ds-cast.3dsx: n3ds-cast.elf
	3dsxtool $< $@
-include $(wildcard $(BUILD)/*.d)
