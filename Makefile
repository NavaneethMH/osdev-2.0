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

$(BUILD_DIR)/kernel.o: kernel.c disk.h heap.h gdt.h elf.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/elf.o: elf.c elf.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/disk.o: disk.c disk.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/heap.o: heap.c heap.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/gdt.o: gdt.c gdt.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(KERNEL): $(BUILD_DIR)/boot.o $(BUILD_DIR)/interrupts.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/disk.o $(BUILD_DIR)/heap.o $(BUILD_DIR)/gdt.o $(BUILD_DIR)/elf.o linker.ld
	$(LD) -m elf_i386 -T linker.ld -nostdlib -o $@ $(BUILD_DIR)/boot.o $(BUILD_DIR)/interrupts.o $(BUILD_DIR)/kernel.o $(BUILD_DIR)/disk.o $(BUILD_DIR)/heap.o $(BUILD_DIR)/gdt.o $(BUILD_DIR)/elf.o
	grub-file --is-x86-multiboot $@

iso: $(KERNEL) grub.cfg
	set -eu; \
	staging=$$(mktemp -d /tmp/tinyos-isodir.XXXXXX); \
	image=$$(mktemp /tmp/tinyos-image.XXXXXX.iso); \
	trap 'rm -rf "$$staging"; rm -f "$$image"' EXIT; \
	mkdir -p "$$staging/boot/grub" "$(ISO_ROOT)/boot/grub"; \
	cp "$(KERNEL)" "$$staging/boot/kernel.elf"; \
	cp grub.cfg "$$staging/boot/grub/grub.cfg"; \
	grub-mkrescue -o "$$image" "$$staging"; \
	xorriso -osirrox on -indev "$$image" -extract /boot/kernel.elf "$$staging/kernel.checked"; \
	cmp "$(KERNEL)" "$$staging/kernel.checked"; \
	xorriso -osirrox on -indev "$$image" -extract /boot/grub/grub.cfg "$$staging/grub.checked"; \
	cmp grub.cfg "$$staging/grub.checked"; \
	cp "$(KERNEL)" "$(ISO_ROOT)/boot/kernel.elf"; \
	cp grub.cfg "$(ISO_ROOT)/boot/grub/grub.cfg"; \
	cp "$$image" "$(ISO)"

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

