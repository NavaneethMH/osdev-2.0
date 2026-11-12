BUILD_DIR := build
ISO_ROOT := $(BUILD_DIR)/isodir
KERNEL := $(BUILD_DIR)/kernel.elf
ISO := $(BUILD_DIR)/tinyos.iso
DISK_IMAGE := disk/tinyos.img
DISK_SIZE_MB := 16

CC := gcc
AS := nasm
LD := ld
CFLAGS := -m32 -std=c11 -ffreestanding -O2 -Wall -Wextra -Werror \
          -fno-stack-protector -fno-pic -fno-pie \
          -fno-asynchronous-unwind-tables -fno-unwind-tables

ifeq ($(DEBUG),1)
CFLAGS += -DHEAP_DEBUG
endif

.PHONY: all iso run debug debug-run clean disk-image

all: iso

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/boot.o: boot.asm | $(BUILD_DIR)
	$(AS) -f elf32 $< -o $@

$(BUILD_DIR)/interrupts.o: interrupts.asm | $(BUILD_DIR)
	$(AS) -f elf32 $< -o $@

$(BUILD_DIR)/kernel.o: kernel.c disk.h heap.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/disk.o: disk.c disk.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/heap.o: heap.c heap.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(BUILD_DIR)/boot.o $(BUILD_DIR)/interrupts.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/disk.o $(BUILD_DIR)/heap.o linker.ld
	$(LD) -m elf_i386 -T linker.ld -nostdlib -o $@ $(BUILD_DIR)/boot.o $(BUILD_DIR)/interrupts.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/disk.o $(BUILD_DIR)/heap.o
	grub-file --is-x86-multiboot $@

iso: $(KERNEL) grub.cfg
	mkdir -p $(ISO_ROOT)/boot/grub
	cp $(KERNEL) $(ISO_ROOT)/boot/kernel.elf
	cp grub.cfg $(ISO_ROOT)/boot/grub/grub.cfg
	grub-mkrescue -o $(ISO) $(ISO_ROOT)

disk-image:
	mkdir -p disk
	if [ ! -f "$(DISK_IMAGE)" ]; then truncate -s $(DISK_SIZE_MB)M "$(DISK_IMAGE)"; fi

run: iso disk-image
	qemu-system-i386 -boot order=d -cdrom $(ISO) -drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk -m 128M

debug:
	$(MAKE) clean
	$(MAKE) DEBUG=1 iso

debug-run: debug disk-image
	qemu-system-i386 -boot order=d -cdrom $(ISO) -drive file=$(DISK_IMAGE),format=raw,if=ide,index=0,media=disk -m 128M

clean:
	rm -rf $(BUILD_DIR)

