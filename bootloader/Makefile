# Stepwhere bootloader for STM32G070CB.
#
#   make                 build build/bootloader.{elf,bin,hex}
#   make CROSS=/path/to/bin/arm-none-eabi-
#
# Needs arm-none-eabi-gcc. The one shipped with the STM32duino core works,
# e.g. ~/.arduino15/packages/STMicroelectronics/tools/xpack-arm-none-eabi-gcc/*/bin/

CROSS   ?= arm-none-eabi-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
SIZE    := $(CROSS)size

LIB     := ../libraries/StepwhereOTA/src
BUILD   := build

SRCS := src/startup.c src/main.c src/boot_core.c \
        $(LIB)/w25q.c $(LIB)/ota_crc32.c $(LIB)/ota_sha256.c $(LIB)/ota_image.c $(LIB)/ota_meta.c $(LIB)/ota_uecc.c

CFLAGS := -mcpu=cortex-m0plus -mthumb -Os -g -std=c11 \
          -Wall -Wextra -Werror \
          -ffunction-sections -fdata-sections -fno-common -MMD -MP \
          -DSTM32G070xx -Icmsis -Isrc -I$(LIB)
LDFLAGS := -mcpu=cortex-m0plus -mthumb -nostartfiles --specs=nano.specs --specs=nosys.specs \
           -Wl,--gc-sections -Wl,-Map=$(BUILD)/bootloader.map -Tstm32g070cb_boot.ld

OBJS := $(patsubst %.c,$(BUILD)/obj/%.o,$(notdir $(SRCS)))
vpath %.c src $(LIB)

all: $(BUILD)/bootloader.bin $(BUILD)/bootloader.hex
	@$(SIZE) $(BUILD)/bootloader.elf

$(BUILD)/obj/%.o: %.c | $(BUILD)/obj
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/bootloader.elf: $(OBJS) stm32g070cb_boot.ld
	$(CC) $(LDFLAGS) $(OBJS) -o $@

$(BUILD)/bootloader.bin: $(BUILD)/bootloader.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/bootloader.hex: $(BUILD)/bootloader.elf
	$(OBJCOPY) -O ihex $< $@

$(BUILD)/obj:
	mkdir -p $@

# Rebuild objects when any included header changes.
-include $(OBJS:.o=.d)

clean:
	rm -rf $(BUILD)

.PHONY: all clean
